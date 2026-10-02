// SPDX-License-Identifier: MPL-2.0

#include "PdfiumContent.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <string_view>
#include <utility>

#include "PdfiumDisplayTransform.h"
#include "core/CheckedArithmetic.hpp"
#include "fpdf_annot.h"
#include "fpdf_edit.h"
#include "fpdf_ppo.h"
#include "fpdf_save.h"
#include "fpdf_text.h"
#include "fpdf_transformpage.h"
#include "pdf/PdfTextLayout.hpp"

namespace rivet::pdf::internal {
namespace {

core::Error makeErr(core::ErrorCode code, std::string message) {
    return core::makeError(code, std::move(message), "pdf");
}

std::unexpected<core::Error> fail(core::ErrorCode code, std::string message) {
    return std::unexpected(makeErr(code, std::move(message)));
}

// RAII for FPDF_DOCUMENT (scratch documents).
class ScopedDocument {
public:
    explicit ScopedDocument(FPDF_DOCUMENT document = nullptr) : document_(document) {}
    ~ScopedDocument() { reset(); }
    ScopedDocument(const ScopedDocument&) = delete;
    ScopedDocument& operator=(const ScopedDocument&) = delete;
    void reset() {
        if (document_ != nullptr) {
            FPDF_CloseDocument(document_);
            document_ = nullptr;
        }
    }
    FPDF_DOCUMENT get() const { return document_; }
    FPDF_DOCUMENT release() { return std::exchange(document_, nullptr); }

private:
    FPDF_DOCUMENT document_;
};

// A page object not yet owned by a page: destroyed unless released.
class OwnedObject {
public:
    explicit OwnedObject(FPDF_PAGEOBJECT object) : object_(object) {}
    ~OwnedObject() {
        if (object_ != nullptr) {
            FPDFPageObj_Destroy(object_);
        }
    }
    OwnedObject(const OwnedObject&) = delete;
    OwnedObject& operator=(const OwnedObject&) = delete;
    FPDF_PAGEOBJECT get() const { return object_; }
    FPDF_PAGEOBJECT release() { return std::exchange(object_, nullptr); }

private:
    FPDF_PAGEOBJECT object_;
};

constexpr int kObjText = FPDF_PAGEOBJ_TEXT;
constexpr int kObjPath = FPDF_PAGEOBJ_PATH;
constexpr int kObjImage = FPDF_PAGEOBJ_IMAGE;
constexpr int kObjShading = FPDF_PAGEOBJ_SHADING;
constexpr int kObjForm = FPDF_PAGEOBJ_FORM;

constexpr double wide(float value) {
    return static_cast<double>(value);
}

PdfContentObjectType toObjectType(int type) {
    switch (type) {
    case kObjText: return PdfContentObjectType::Text;
    case kObjImage: return PdfContentObjectType::Image;
    case kObjPath: return PdfContentObjectType::Path;
    case kObjShading: return PdfContentObjectType::Shading;
    case kObjForm: return PdfContentObjectType::Form;
    default: return PdfContentObjectType::Unknown;
    }
}

// --- Memory serialization -------------------------------------------------------

constexpr std::size_t kMaxSerializedBytes = 512u * 1024u * 1024u;

struct MemoryWriter final : FPDF_FILEWRITE {
    std::vector<std::uint8_t>* out = nullptr;
    bool failed = false;
};

int writeToMemory(FPDF_FILEWRITE* self, const void* data, unsigned long size) noexcept {
    auto* writer = static_cast<MemoryWriter*>(self);
    if (writer->failed) {
        return 0;
    }
    try {
        if (size > kMaxSerializedBytes || writer->out->size() > kMaxSerializedBytes - size) {
            writer->failed = true;
            return 0;
        }
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        writer->out->insert(writer->out->end(), bytes, bytes + size);
    } catch (...) {
        writer->failed = true;
        return 0;
    }
    return 1;
}

core::Status saveToMemory(FPDF_DOCUMENT document, std::vector<std::uint8_t>& out) {
    MemoryWriter writer;
    writer.version = 1;
    writer.WriteBlock = &writeToMemory;
    writer.out = &out;
    const bool saved = FPDF_SaveAsCopy(document, &writer, FPDF_NO_INCREMENTAL) != 0;
    if (!saved || writer.failed || out.empty()) {
        return fail(core::ErrorCode::Internal, "PDFium failed to serialize the edited page");
    }
    return core::ok();
}

// --- Text collection -----------------------------------------------------------

struct ObjectText {
    std::u32string text;
    bool mapError = false;
    char32_t pendingHigh = 0;
};

// One linear pass over the text page: the text of every text object. (The
// per-object FPDFTextObj_GetText rescans all characters, quadratic on
// text-heavy pages.) Generated characters (separators PDFium inserts between
// objects) and C0 controls are dropped.
std::unordered_map<FPDF_PAGEOBJECT, ObjectText> collectObjectTexts(FPDF_TEXTPAGE textPage) {
    std::unordered_map<FPDF_PAGEOBJECT, ObjectText> texts;
    const int count = FPDFText_CountChars(textPage);
    for (int i = 0; i < count; ++i) {
        if (FPDFText_IsGenerated(textPage, i) == 1) {
            continue;
        }
        const FPDF_PAGEOBJECT object = FPDFText_GetTextObject(textPage, i);
        if (object == nullptr) {
            continue;
        }
        ObjectText& entry = texts[object];
        if (FPDFText_HasUnicodeMapError(textPage, i) == 1) {
            entry.mapError = true;
        }
        const unsigned int unit = FPDFText_GetUnicode(textPage, i);
        if (unit == 0 || unit == 0xFFFE || unit == 0xFFFF || unit == 0xFFFD) {
            entry.mapError = true;
            continue;
        }
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            entry.pendingHigh = static_cast<char32_t>(unit);
            continue;
        }
        char32_t code = static_cast<char32_t>(unit);
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            if (entry.pendingHigh == 0) {
                entry.mapError = true;
                continue;
            }
            code = 0x10000 + ((entry.pendingHigh - 0xD800) << 10) + (static_cast<char32_t>(unit) - 0xDC00);
            entry.pendingHigh = 0;
        }
        if (code < 0x20) {
            continue;
        }
        entry.text.push_back(code);
    }
    return texts;
}

bool isSpaceLike(char32_t c) {
    return c <= 0x20 || c == 0xA0;
}

std::u32string withoutSpaces(std::u32string_view text) {
    std::u32string out;
    for (const char32_t c : text) {
        if (!isSpaceLike(c)) {
            out.push_back(c);
        }
    }
    return out;
}

// --- Fonts -----------------------------------------------------------------------

constexpr std::string_view kStandard14[] = {
    "Helvetica",  "Helvetica-Bold",  "Helvetica-Oblique", "Helvetica-BoldOblique",
    "Times-Roman", "Times-Bold",     "Times-Italic",      "Times-BoldItalic",
    "Courier",    "Courier-Bold",    "Courier-Oblique",   "Courier-BoldOblique",
    "Symbol",     "ZapfDingbats",
};

std::string readFontString(FPDF_FONT font, size_t (*getter)(FPDF_FONT, char*, size_t)) {
    const std::size_t needed = getter(font, nullptr, 0);
    if (needed < 2 || needed > 1024) {
        return {};
    }
    std::string buffer(needed, '\0');
    const std::size_t written = getter(font, buffer.data(), buffer.size());
    if (written < 2) {
        return {};
    }
    buffer.resize(std::min(written, needed) - 1); // drop the NUL
    return buffer;
}

bool hasSubsetPrefix(std::string_view name) {
    if (name.size() < 8 || name[6] != '+') {
        return false;
    }
    for (std::size_t i = 0; i < 6; ++i) {
        if (name[i] < 'A' || name[i] > 'Z') {
            return false;
        }
    }
    return true;
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

// Type 3 detection: PDFium reports Type 3 fonts as "embedded" (CPDF_Font::
// IsEmbedded is true for them) but they carry no font program, which every
// other embedded font does. This is the only public-API signal.
bool isType3(FPDF_FONT font) {
    if (FPDFFont_GetIsEmbedded(font) != 1) {
        return false;
    }
    std::size_t size = 0;
    if (FPDFFont_GetFontData(font, nullptr, 0, &size) == 0) {
        return true;
    }
    return size == 0;
}

PdfFontInfo describeFont(FPDF_FONT font) {
    PdfFontInfo info;
    if (font == nullptr) {
        return info;
    }
    info.baseName = readFontString(font, &FPDFFont_GetBaseFontName);
    info.familyName = readFontString(font, &FPDFFont_GetFamilyName);
    info.type3 = isType3(font);
    info.embedded = !info.type3 && FPDFFont_GetIsEmbedded(font) == 1;
    info.subset = info.embedded && hasSubsetPrefix(info.baseName);
    const std::string_view bare = info.subset ? std::string_view(info.baseName).substr(7) : info.baseName;
    info.standard14 = !info.embedded && !info.type3 &&
                      std::find(std::begin(kStandard14), std::end(kStandard14), bare) != std::end(kStandard14);

    const int flags = std::max(0, FPDFFont_GetFlags(font));
    const int weight = FPDFFont_GetWeight(font);
    int angle = 0;
    const bool haveAngle = FPDFFont_GetItalicAngle(font, &angle) != 0;
    // PDFium writes no serif flag for fonts it loads from bytes, so the
    // bundled serif (Tinos) and the common serif names are matched by name.
    info.monospace = (flags & 1) != 0 || contains(bare, "Courier") || contains(bare, "Mono") ||
                     contains(bare, "Cousine");
    info.serif = (flags & 2) != 0 || contains(bare, "Times") || contains(bare, "Tinos") ||
                 (contains(bare, "Serif") && !contains(bare, "Sans")) || contains(bare, "Georgia") ||
                 contains(bare, "Garamond") || contains(bare, "Cambria") || contains(bare, "Palatino") ||
                 contains(bare, "Bookman");
    info.italic = (flags & 64) != 0 || (haveAngle && angle != 0) || contains(bare, "Italic") || contains(bare, "Oblique");
    info.bold = weight >= 600 || (flags & (1 << 18)) != 0 || contains(bare, "Bold") || contains(bare, "Black");
    return info;
}

} // namespace

FontCache::~FontCache() {
    for (FPDF_FONT& font : fonts_) {
        if (font != nullptr) {
            FPDFFont_Close(font);
            font = nullptr;
        }
    }
}

FPDF_FONT FontCache::get(PdfBundledFont face) {
    const auto index = static_cast<std::size_t>(face);
    if (index >= fonts_.size()) {
        return nullptr;
    }
    if (fonts_[index] == nullptr) {
        const std::span<const std::uint8_t> data = bundledFontData(face);
        fonts_[index] = FPDFText_LoadFont(document_, data.data(), static_cast<std::uint32_t>(data.size()),
                                          FPDF_FONT_TRUETYPE, /*cid=*/1);
    }
    return fonts_[index];
}

// --- Extraction ---------------------------------------------------------------------

core::Result<PdfPageContent> extractPageContent(FPDF_PAGE page) {
    PdfPageContent content;
    const int count = FPDFPage_CountObjects(page);
    if (count < 0) {
        return fail(core::ErrorCode::InvalidDocument, "PDFium could not enumerate the page objects");
    }
    if (static_cast<std::size_t>(count) > kMaxContentObjectsPerPage) {
        content.truncated = true;
        return content;
    }

    std::vector<FPDF_PAGEOBJECT> handles;
    handles.reserve(static_cast<std::size_t>(count));
    bool haveText = false;
    for (int i = 0; i < count; ++i) {
        const FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, i);
        if (object == nullptr) {
            return fail(core::ErrorCode::InvalidDocument, "PDFium returned a null page object");
        }
        handles.push_back(object);
        haveText = haveText || FPDFPageObj_GetType(object) == kObjText;
    }

    ScopedTextPage textPage(haveText ? FPDFText_LoadPage(page) : nullptr);
    std::unordered_map<FPDF_PAGEOBJECT, ObjectText> texts;
    if (textPage.get() != nullptr) {
        texts = collectObjectTexts(textPage.get());
    }
    std::unordered_map<FPDF_FONT, PdfFontInfo> fontInfos;

    content.objects.reserve(handles.size());
    for (std::size_t i = 0; i < handles.size(); ++i) {
        const FPDF_PAGEOBJECT object = handles[i];
        PdfContentObject out;
        out.index = static_cast<std::uint32_t>(i);
        out.origin = PdfContentOrigin{PdfContentOrigin::Kind::Source, static_cast<std::uint32_t>(i), 0};
        const int type = FPDFPageObj_GetType(object);
        out.type = toObjectType(type);

        FS_MATRIX matrix{};
        if (FPDFPageObj_GetMatrix(object, &matrix) != 0) {
            out.matrix = core::Matrix{wide(matrix.a), wide(matrix.b), wide(matrix.c), wide(matrix.d), wide(matrix.e), wide(matrix.f)};
        }

        float left = 0.0F;
        float bottom = 0.0F;
        float right = 0.0F;
        float top = 0.0F;
        if (FPDFPageObj_GetBounds(object, &left, &bottom, &right, &top) != 0) {
            out.bounds = PdfBox{wide(left), wide(bottom), wide(right), wide(top)};
        }
        out.quad = {PdfPoint{out.bounds.left, out.bounds.bottom}, PdfPoint{out.bounds.right, out.bounds.bottom},
                    PdfPoint{out.bounds.right, out.bounds.top}, PdfPoint{out.bounds.left, out.bounds.top}};
        if (type == kObjText || type == kObjImage) {
            FS_QUADPOINTSF quad{};
            if (FPDFPageObj_GetRotatedBounds(object, &quad) != 0) {
                // Bottom-left, bottom-right, top-right, top-left of the
                // object's own rectangle transformed by its matrix.
                out.quad = {PdfPoint{wide(quad.x1), wide(quad.y1)}, PdfPoint{wide(quad.x2), wide(quad.y2)},
                            PdfPoint{wide(quad.x3), wide(quad.y3)}, PdfPoint{wide(quad.x4), wide(quad.y4)}};
            }
        }

        if (FPDF_CLIPPATH clip = FPDFPageObj_GetClipPath(object); clip != nullptr) {
            out.hasClip = FPDFClipPath_CountPaths(clip) > 0;
        }
        out.hasMarkedContent = FPDFPageObj_CountMarks(object) > 0;

        unsigned int r = 0;
        unsigned int g = 0;
        unsigned int b = 0;
        unsigned int a = 0;
        const auto readFill = [&]() -> std::optional<PdfColor> {
            if (FPDFPageObj_GetFillColor(object, &r, &g, &b, &a) == 0) {
                return std::nullopt;
            }
            return PdfColor{static_cast<float>(r) / 255.0F, static_cast<float>(g) / 255.0F,
                            static_cast<float>(b) / 255.0F};
        };
        const auto readStroke = [&]() -> std::optional<PdfColor> {
            if (FPDFPageObj_GetStrokeColor(object, &r, &g, &b, &a) == 0) {
                return std::nullopt;
            }
            return PdfColor{static_cast<float>(r) / 255.0F, static_cast<float>(g) / 255.0F,
                            static_cast<float>(b) / 255.0F};
        };

        switch (type) {
        case kObjText: {
            const auto found = texts.find(object);
            if (found != texts.end()) {
                out.text = encodeUtf8(found->second.text);
                out.textUnmappable = found->second.mapError;
            }
            if (out.text.empty() && out.bounds.width() > 0.0) {
                out.textUnmappable = true; // glyphs without any Unicode mapping
            }
            float size = 0.0F;
            if (FPDFTextObj_GetFontSize(object, &size) != 0) {
                const double scale = std::sqrt(std::fabs(out.matrix.a * out.matrix.d - out.matrix.b * out.matrix.c));
                out.fontSize = static_cast<double>(size) * scale;
            }
            if (const FPDF_FONT font = FPDFTextObj_GetFont(object); font != nullptr) {
                auto info = fontInfos.find(font);
                if (info == fontInfos.end()) {
                    info = fontInfos.emplace(font, describeFont(font)).first;
                }
                out.font = info->second;
            }
            const int mode = static_cast<int>(FPDFTextObj_GetTextRenderMode(object));
            out.renderMode = std::clamp(mode, 0, 7);
            if (mode == 0 || mode == 2 || mode == 4 || mode == 6) {
                out.fill = readFill();
            }
            if (mode == 1 || mode == 2 || mode == 5 || mode == 6) {
                out.stroke = readStroke();
            }
            break;
        }
        case kObjPath: {
            const int segments = FPDFPath_CountSegments(object);
            out.segmentCount = segments > 0 ? static_cast<std::uint32_t>(segments) : 0;
            int fill = FPDF_FILLMODE_NONE;
            FPDF_BOOL stroke = 0;
            if (FPDFPath_GetDrawMode(object, &fill, &stroke) != 0) {
                if (fill != FPDF_FILLMODE_NONE) {
                    out.fill = readFill();
                }
                if (stroke != 0) {
                    out.stroke = readStroke();
                }
            }
            break;
        }
        case kObjImage: {
            unsigned int width = 0;
            unsigned int height = 0;
            if (FPDFImageObj_GetImagePixelSize(object, &width, &height) != 0) {
                out.pixelWidth = width;
                out.pixelHeight = height;
            }
            break;
        }
        case kObjForm: {
            const int children = FPDFFormObj_CountObjects(object);
            out.childCount = children > 0 ? static_cast<std::uint32_t>(children) : 0;
            break;
        }
        default:
            break;
        }
        content.objects.push_back(std::move(out));
    }
    return content;
}

// --- Regeneration probe -------------------------------------------------------------

namespace {

// Per-channel difference (0-255) above which a pixel is a hard failure.
constexpr int kProbeHardDiff = 32;
// Per-channel difference tolerated as anti-aliasing noise.
constexpr int kProbeSoftDiff = 2;
// Fraction of pixels allowed above kProbeSoftDiff.
constexpr double kProbeSoftFraction = 0.0005;
// Longest side of the probe renderings, pixels.
constexpr double kProbeMaxSide = 512.0;
// Bounds tolerance after regeneration, points.
constexpr double kProbeBoundsTolerance = 0.25;

struct ProbeBitmap {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels; // BGRA
};

std::optional<ProbeBitmap> renderForProbe(FPDF_PAGE page) {
    const double pageWidth = static_cast<double>(FPDF_GetPageWidthF(page));
    const double pageHeight = static_cast<double>(FPDF_GetPageHeightF(page));
    if (!(pageWidth > 0.0) || !(pageHeight > 0.0)) {
        return std::nullopt;
    }
    const double scale = kProbeMaxSide / std::max(pageWidth, pageHeight);
    ProbeBitmap bitmap;
    bitmap.width = std::max(1, static_cast<int>(std::lround(pageWidth * scale)));
    bitmap.height = std::max(1, static_cast<int>(std::lround(pageHeight * scale)));
    bitmap.pixels.assign(static_cast<std::size_t>(bitmap.width) * static_cast<std::size_t>(bitmap.height) * 4u, 0xFF);
    FPDF_BITMAP handle = FPDFBitmap_CreateEx(bitmap.width, bitmap.height, FPDFBitmap_BGRA, bitmap.pixels.data(),
                                             bitmap.width * 4);
    if (handle == nullptr) {
        return std::nullopt;
    }
    FPDFBitmap_FillRect(handle, 0, 0, bitmap.width, bitmap.height, 0xFFFFFFFFu);
    FPDF_RenderPageBitmap(handle, page, 0, 0, bitmap.width, bitmap.height, 0, 0);
    FPDFBitmap_Destroy(handle);
    return bitmap;
}

struct ObjectRecord {
    int type = 0;
    double left = 0.0;
    double bottom = 0.0;
    double right = 0.0;
    double top = 0.0;
};

} // namespace

RegenerationProbe probeRegeneration(FPDF_DOCUMENT source, FPDF_DOCUMENT referenceDocument, int pageIndex) {
    const auto unsafe = [](std::string issue) { return RegenerationProbe{false, std::move(issue)}; };
    try {
        ScopedDocument scratch(FPDF_CreateNewDocument());
        if (scratch.get() == nullptr) {
            return unsafe("fidelity probe could not create a scratch document");
        }
        const int indices[1] = {pageIndex};
        if (FPDF_ImportPagesByIndex(scratch.get(), source, indices, 1, 0) == 0) {
            return unsafe("fidelity probe could not import the page");
        }

        std::vector<std::uint8_t> serialized;
        std::vector<ObjectRecord> before;
        std::optional<ProbeBitmap> reference;
        {
            ScopedPage page(FPDF_LoadPage(scratch.get(), 0));
            if (page.get() == nullptr) {
                return unsafe("fidelity probe could not load the page");
            }
            const int count = FPDFPage_CountObjects(page.get());
            if (count < 0 || static_cast<std::size_t>(count) > kMaxContentObjectsPerPage) {
                return unsafe("page has too many objects");
            }
            {
                // Edits are indexed against the page as the file stores it
                // but applied to the imported copy: both must agree (an
                // import that drops unreadable content would shift them).
                ScopedPage stored(FPDF_LoadPage(referenceDocument, pageIndex));
                if (stored.get() == nullptr || FPDFPage_CountObjects(stored.get()) != count) {
                    return unsafe("importing the page changes its objects");
                }
            }
            if (count == 0) {
                return RegenerationProbe{};
            }
            std::vector<FPDF_PAGEOBJECT> handles;
            for (int i = 0; i < count; ++i) {
                const FPDF_PAGEOBJECT object = FPDFPage_GetObject(page.get(), i);
                if (object == nullptr) {
                    return unsafe("fidelity probe could not read the page objects");
                }
                const int type = FPDFPageObj_GetType(object);
                if (type == kObjShading) {
                    return unsafe("page contains shading objects");
                }
                if (type == kObjText) {
                    const int mode = static_cast<int>(FPDFTextObj_GetTextRenderMode(object));
                    if (mode >= 4) {
                        return unsafe("page contains text used as a clip");
                    }
                }
                ObjectRecord record;
                record.type = type;
                float l = 0.0F;
                float b = 0.0F;
                float r = 0.0F;
                float t = 0.0F;
                if (FPDFPageObj_GetBounds(object, &l, &b, &r, &t) != 0) {
                    record = ObjectRecord{type, wide(l), wide(b), wide(r), wide(t)};
                }
                before.push_back(record);
                handles.push_back(object);
            }
            {
                ScopedPage original(FPDF_LoadPage(referenceDocument, pageIndex));
                if (original.get() != nullptr) {
                    // The page as the file stores it must have the objects
                    // the scratch copy has: edits are indexed against the
                    // stored page but applied to the imported one.
                    if (FPDFPage_CountObjects(original.get()) != count) {
                        return unsafe("importing the page changes its objects");
                    }
                    for (int i = 0; i < count; ++i) {
                        const FPDF_PAGEOBJECT object = FPDFPage_GetObject(original.get(), i);
                        if (object == nullptr || FPDFPageObj_GetType(object) != before[static_cast<std::size_t>(i)].type) {
                            return unsafe("importing the page changes its objects");
                        }
                    }
                    reference = renderForProbe(original.get());
                }
            }
            if (!reference.has_value()) {
                return unsafe("fidelity probe could not render the page");
            }
            // Mark every object dirty (and so every content stream) with an
            // identity transform; GenerateContent then rewrites them all.
            const FS_MATRIX identity{1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F};
            for (const FPDF_PAGEOBJECT object : handles) {
                if (FPDFPageObj_TransformF(object, &identity) == 0) {
                    return unsafe("page objects cannot be rewritten");
                }
            }
            if (FPDFPage_GenerateContent(page.get()) == 0) {
                return unsafe("content regeneration failed");
            }
            if (!saveToMemory(scratch.get(), serialized).has_value()) {
                return unsafe("regenerated page could not be serialized");
            }
        }
        scratch.reset();

        ScopedDocument reloaded(FPDF_LoadMemDocument64(serialized.data(), serialized.size(), nullptr));
        if (reloaded.get() == nullptr) {
            return unsafe("regenerated page could not be reloaded");
        }
        ScopedPage page(FPDF_LoadPage(reloaded.get(), 0));
        if (page.get() == nullptr) {
            return unsafe("regenerated page could not be loaded");
        }
        const int count = FPDFPage_CountObjects(page.get());
        if (count < 0 || static_cast<std::size_t>(count) != before.size()) {
            return unsafe("object structure changes when the content is rewritten");
        }
        for (int i = 0; i < count; ++i) {
            const FPDF_PAGEOBJECT object = FPDFPage_GetObject(page.get(), i);
            const ObjectRecord& expected = before[static_cast<std::size_t>(i)];
            if (object == nullptr || FPDFPageObj_GetType(object) != expected.type) {
                return unsafe("object types change when the content is rewritten");
            }
            float l = 0.0F;
            float b = 0.0F;
            float r = 0.0F;
            float t = 0.0F;
            if (FPDFPageObj_GetBounds(object, &l, &b, &r, &t) != 0) {
                if (std::fabs(wide(l) - expected.left) > kProbeBoundsTolerance ||
                    std::fabs(wide(b) - expected.bottom) > kProbeBoundsTolerance ||
                    std::fabs(wide(r) - expected.right) > kProbeBoundsTolerance ||
                    std::fabs(wide(t) - expected.top) > kProbeBoundsTolerance) {
                    return unsafe("object geometry changes when the content is rewritten");
                }
            }
        }
        const std::optional<ProbeBitmap> after = renderForProbe(page.get());
        if (!after.has_value() || after->width != reference->width || after->height != reference->height) {
            return unsafe("regenerated page could not be rendered");
        }
        const std::size_t pixelCount = static_cast<std::size_t>(after->width) * static_cast<std::size_t>(after->height);
        std::size_t soft = 0;
        for (std::size_t p = 0; p < pixelCount; ++p) {
            int worst = 0;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const int d = std::abs(static_cast<int>(reference->pixels[p * 4 + channel]) -
                                       static_cast<int>(after->pixels[p * 4 + channel]));
                worst = std::max(worst, d);
            }
            if (worst > kProbeHardDiff) {
                return unsafe("rewriting the content changes how the page looks");
            }
            if (worst > kProbeSoftDiff) {
                ++soft;
            }
        }
        if (static_cast<double>(soft) > kProbeSoftFraction * static_cast<double>(pixelCount)) {
            return unsafe("rewriting the content slightly changes how the page looks");
        }
        return RegenerationProbe{};
    } catch (const std::exception&) {
        return unsafe("fidelity probe failed");
    }
}

// --- Applying edits ------------------------------------------------------------------

namespace {

std::vector<FPDF_WCHAR> toUtf16(std::u32string_view text) {
    std::vector<FPDF_WCHAR> out;
    out.reserve(text.size() + 1);
    for (const char32_t c : text) {
        if (c >= 0x10000) {
            const char32_t v = c - 0x10000;
            out.push_back(static_cast<FPDF_WCHAR>(0xD800 + (v >> 10)));
            out.push_back(static_cast<FPDF_WCHAR>(0xDC00 + (v & 0x3FF)));
        } else {
            out.push_back(static_cast<FPDF_WCHAR>(c));
        }
    }
    out.push_back(0);
    return out;
}

FS_MATRIX toFs(const core::Matrix& m) {
    return FS_MATRIX{static_cast<float>(m.a), static_cast<float>(m.b), static_cast<float>(m.c),
                     static_cast<float>(m.d), static_cast<float>(m.tx), static_cast<float>(m.ty)};
}

core::Matrix fromFs(const FS_MATRIX& m) {
    return core::Matrix{wide(m.a), wide(m.b), wide(m.c), wide(m.d), wide(m.e), wide(m.f)};
}

unsigned int toByte(float v) {
    return static_cast<unsigned int>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F));
}

// Shared state of one applyContentEdits run.
struct ApplyContext {
    FPDF_DOCUMENT document = nullptr;
    FPDF_PAGE page = nullptr;
    FontCache* fonts = nullptr;
    std::vector<FPDF_PAGEOBJECT> sources; // source index -> handle
    std::vector<int> types;
    std::vector<FPDF_PAGEOBJECT> pendingRemoval;
    std::unordered_map<FPDF_PAGEOBJECT, PdfContentOrigin> originOf;
    std::unordered_map<FPDF_PAGEOBJECT, std::uint64_t> tagOf;
    std::unordered_set<std::uint64_t> substituted;
    std::size_t createdLines = 0;
};

// In-memory FPDF_FILEACCESS over a JPEG.
struct JpegSource {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

int readJpegBlock(void* param, unsigned long position, unsigned char* buffer, unsigned long size) {
    const auto* source = static_cast<const JpegSource*>(param);
    if (position > source->size || size > source->size - position) {
        return 0;
    }
    std::memcpy(buffer, source->data + position, size);
    return 1;
}

std::vector<std::uint8_t> rawImageData(FPDF_PAGEOBJECT image) {
    const unsigned long size = FPDFImageObj_GetImageDataRaw(image, nullptr, 0);
    std::vector<std::uint8_t> data(size);
    if (size > 0) {
        FPDFImageObj_GetImageDataRaw(image, data.data(), size);
    }
    return data;
}

// PDFium replaces an image IN PLACE on its shared CPDF_Image, so another
// image object of the page showing the same XObject would change too. Detect
// that (same pixel size and identical raw data) and refuse: no public API can
// give the object its own image.
bool sharesImage(const ApplyContext& context, std::size_t index) {
    unsigned int width = 0;
    unsigned int height = 0;
    if (FPDFImageObj_GetImagePixelSize(context.sources[index], &width, &height) == 0) {
        return false;
    }
    std::optional<std::vector<std::uint8_t>> mine;
    for (std::size_t other = 0; other < context.sources.size(); ++other) {
        if (other == index || context.types[other] != kObjImage) {
            continue;
        }
        unsigned int otherWidth = 0;
        unsigned int otherHeight = 0;
        if (FPDFImageObj_GetImagePixelSize(context.sources[other], &otherWidth, &otherHeight) == 0 ||
            otherWidth != width || otherHeight != height) {
            continue;
        }
        if (!mine.has_value()) {
            mine = rawImageData(context.sources[index]);
        }
        if (!mine->empty() && rawImageData(context.sources[other]) == *mine) {
            return true;
        }
    }
    return false;
}

core::Status replaceImage(ApplyContext& context, std::size_t index, const PdfImageData& data,
                          const core::Matrix& frame, FPDF_PAGEOBJECT object, core::Matrix& result) {
    if (sharesImage(context, index)) {
        return fail(core::ErrorCode::InvalidArgument,
                    "the image shares its data with another object and cannot be replaced on its own");
    }
    FPDF_PAGE pages[1] = {context.page};
    double pixelWidth = data.width;
    double pixelHeight = data.height;
    if (data.format == PdfImageData::Format::Jpeg) {
        JpegSource source{data.bytes.data(), data.bytes.size()};
        FPDF_FILEACCESS access{};
        access.m_FileLen = static_cast<unsigned long>(data.bytes.size());
        access.m_GetBlock = &readJpegBlock;
        access.m_Param = &source;
        if (FPDFImageObj_LoadJpegFileInline(pages, 1, object, &access) == 0) {
            return fail(core::ErrorCode::InvalidArgument, "the replacement JPEG could not be decoded");
        }
        unsigned int w = 0;
        unsigned int h = 0;
        if (FPDFImageObj_GetImagePixelSize(object, &w, &h) == 0 || w == 0 || h == 0) {
            return fail(core::ErrorCode::InvalidArgument, "the replacement JPEG has no pixels");
        }
        if (w > kMaxImageSide || h > kMaxImageSide ||
            static_cast<std::uint64_t>(w) * h > kMaxImagePixels) {
            return fail(core::ErrorCode::InvalidArgument, "the replacement JPEG is too large");
        }
        pixelWidth = w;
        pixelHeight = h;
    } else {
        // PDFium only reads the buffer; the const_cast is not written through.
        FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(static_cast<int>(data.width), static_cast<int>(data.height),
                                                 FPDFBitmap_BGRA, const_cast<std::uint8_t*>(data.bytes.data()),
                                                 static_cast<int>(data.stride));
        if (bitmap == nullptr) {
            return fail(core::ErrorCode::InvalidArgument, "the replacement bitmap is not usable");
        }
        const bool ok = FPDFImageObj_SetBitmap(pages, 1, object, bitmap) != 0;
        FPDFBitmap_Destroy(bitmap);
        if (!ok) {
            return fail(core::ErrorCode::Internal, "PDFium could not set the replacement bitmap");
        }
    }

    // Fit the new pixels (aspect ratio kept) into the old frame, centered.
    const double frameWidth = std::hypot(frame.a, frame.b);
    const double frameHeight = std::hypot(frame.c, frame.d);
    if (!(frameWidth > 0.0) || !(frameHeight > 0.0) || !(pixelWidth > 0.0) || !(pixelHeight > 0.0)) {
        return fail(core::ErrorCode::InvalidArgument, "the image frame is degenerate");
    }
    const double aspect = pixelWidth / pixelHeight;
    double fitWidth = frameWidth;
    double fitHeight = frameHeight;
    if (frameWidth / frameHeight >= aspect) {
        fitWidth = aspect * frameHeight;
    } else {
        fitHeight = frameWidth / aspect;
    }
    const double sx = fitWidth / frameWidth;
    const double sy = fitHeight / frameHeight;
    const core::Matrix local{sx, 0.0, 0.0, sy, (1.0 - sx) / 2.0, (1.0 - sy) / 2.0};
    result = frame * local;
    return core::ok();
}

// --- Text blocks ----------------------------------------------------------------------

struct BlockFont {
    FPDF_FONT font = nullptr;
    bool fromObject = false; // the font of a source object (reuse allowed)
    bool substituted = false;
    PdfTextLayoutResult layout;
};

PdfAdvanceFunction advanceFor(FPDF_FONT font, double size) {
    return [font, size](char32_t c) -> double {
        float width = 0.0F;
        if (FPDFFont_GetGlyphWidth(font, static_cast<std::uint32_t>(c), static_cast<float>(size), &width) == 0) {
            return 0.0;
        }
        return width > 0.0F ? static_cast<double>(width) : 0.0;
    };
}

// Every visible code point must have a positive advance in the font (a
// missing glyph of a subset font has width 0), and nothing outside the BMP
// (PDFium converts through wchar_t per code point, which is fine, but simple
// fonts cannot encode it anyway).
bool allMeasurable(FPDF_FONT font, double size, const PdfTextLayoutResult& layout) {
    const PdfAdvanceFunction advance = advanceFor(font, size);
    for (const PdfTextLine& line : layout.lines) {
        for (const char32_t c : line.text) {
            if (!isSpaceLike(c) && !(advance(c) > 0.0)) {
                return false;
            }
        }
    }
    return true;
}

// The ADR-0015/0017 round trip: set the lines on temporary text objects, read
// them back through a text page and compare (ignoring whitespace). PDFium
// reports no error for characters a font cannot encode.
bool roundTripOk(ApplyContext& context, FPDF_FONT font, double size, const PdfTextLayoutResult& layout) {
    std::vector<FPDF_PAGEOBJECT> temporaries;
    std::vector<std::u32string> expected;
    bool ok = true;
    for (const PdfTextLine& line : layout.lines) {
        if (line.text.empty()) {
            continue;
        }
        OwnedObject object(FPDFPageObj_CreateTextObj(context.document, font, static_cast<float>(size)));
        if (object.get() == nullptr) {
            ok = false;
            break;
        }
        const std::vector<FPDF_WCHAR> utf16 = toUtf16(line.text);
        if (FPDFText_SetText(object.get(), utf16.data()) == 0) {
            ok = false;
            break;
        }
        const FPDF_PAGEOBJECT handle = object.release();
        FPDFPage_InsertObject(context.page, handle);
        temporaries.push_back(handle);
        expected.push_back(withoutSpaces(line.text));
    }
    if (ok && !temporaries.empty()) {
        ScopedTextPage textPage(FPDFText_LoadPage(context.page));
        if (textPage.get() == nullptr) {
            ok = false;
        } else {
            const auto texts = collectObjectTexts(textPage.get());
            for (std::size_t i = 0; i < temporaries.size() && ok; ++i) {
                const auto found = texts.find(temporaries[i]);
                ok = found != texts.end() && !found->second.mapError &&
                     withoutSpaces(found->second.text) == expected[i];
            }
        }
    }
    for (const FPDF_PAGEOBJECT handle : temporaries) {
        FPDFPage_RemoveObject(context.page, handle);
        FPDFPageObj_Destroy(handle);
    }
    return ok;
}

core::Status applyTextBlock(ApplyContext& context, const PdfTextBlockEdit& block) {
    std::vector<FPDF_PAGEOBJECT> members;
    for (const std::uint32_t member : block.members) {
        if (context.types[member] != kObjText) {
            return fail(core::ErrorCode::InvalidArgument, "a text block member is not a text object");
        }
        members.push_back(context.sources[member]);
    }

    BlockFont chosen;
    if (block.font.kind == PdfFontRef::Kind::FromObject) {
        if (context.types[block.font.sourceIndex] != kObjText) {
            return fail(core::ErrorCode::InvalidArgument, "the font object of a text block is not a text object");
        }
        const FPDF_FONT sourceFont = FPDFTextObj_GetFont(context.sources[block.font.sourceIndex]);
        if (sourceFont != nullptr && !isType3(sourceFont)) {
            PdfTextLayoutResult layout =
                layoutTextBlock(block.text, block.wrapWidth, advanceFor(sourceFont, block.fontSize));
            if (allMeasurable(sourceFont, block.fontSize, layout) &&
                roundTripOk(context, sourceFont, block.fontSize, layout)) {
                chosen.font = sourceFont;
                chosen.fromObject = true;
                chosen.layout = std::move(layout);
            }
        }
        chosen.substituted = !chosen.fromObject;
    }
    if (chosen.font == nullptr) {
        chosen.font = context.fonts->get(block.font.fallback);
        if (chosen.font == nullptr) {
            return fail(core::ErrorCode::Internal, "PDFium could not load the bundled font");
        }
        chosen.layout = layoutTextBlock(block.text, block.wrapWidth, advanceFor(chosen.font, block.fontSize));
        for (const PdfTextLine& line : chosen.layout.lines) {
            for (const char32_t c : line.text) {
                if (!bundledFontCovers(block.font.fallback, c)) {
                    return fail(core::ErrorCode::InvalidArgument,
                                "the text contains characters the bundled font cannot display");
                }
            }
        }
        if (!allMeasurable(chosen.font, block.fontSize, chosen.layout)) {
            return fail(core::ErrorCode::Internal, "the bundled font could not measure the text");
        }
    }
    if (chosen.substituted) {
        context.substituted.insert(block.tag);
    }

    std::size_t nonEmpty = 0;
    for (const PdfTextLine& line : chosen.layout.lines) {
        nonEmpty += line.text.empty() ? 0 : 1;
    }
    if (nonEmpty > kMaxContentObjectsPerPage - context.createdLines) {
        return fail(core::ErrorCode::InvalidArgument, "the text needs too many lines");
    }
    context.createdLines += nonEmpty;

    // Appearance of created lines: the first member's render mode when it is a
    // visible-or-invisible fill/stroke mode, else fill.
    int renderMode = 0;
    if (!members.empty()) {
        const int mode = static_cast<int>(FPDFTextObj_GetTextRenderMode(members.front()));
        if (mode >= 0 && mode <= 3) {
            renderMode = mode;
        }
    }
    const unsigned int red = toByte(block.color.r);
    const unsigned int green = toByte(block.color.g);
    const unsigned int blue = toByte(block.color.b);
    const double size = block.fontSize;

    std::vector<bool> memberUsed(members.size(), false);
    for (std::size_t k = 0; k < chosen.layout.lines.size(); ++k) {
        const PdfTextLine& line = chosen.layout.lines[k];
        if (line.text.empty()) {
            continue;
        }
        const std::vector<FPDF_WCHAR> utf16 = toUtf16(line.text);
        const core::Point origin = block.placement.map(core::Point{0.0, -static_cast<double>(k) * block.lineAdvance});

        const bool reuse = chosen.fromObject && k < members.size() && FPDFTextObj_GetFont(members[k]) == chosen.font;
        if (reuse) {
            const FPDF_PAGEOBJECT object = members[k];
            if (FPDFText_SetText(object, utf16.data()) == 0) {
                return fail(core::ErrorCode::Internal, "PDFium could not set the text");
            }
            float tf = 0.0F;
            FPDFTextObj_GetFontSize(object, &tf);
            double scale = 1.0;
            if (tf > 1e-6F) {
                scale = size / static_cast<double>(tf);
            } else {
                FPDFTextObj_SetFontSize(object, static_cast<float>(size));
            }
            const core::Matrix matrix{block.placement.a * scale, block.placement.b * scale,
                                      block.placement.c * scale, block.placement.d * scale, origin.x, origin.y};
            const FS_MATRIX fs = toFs(matrix);
            if (FPDFPageObj_SetMatrix(object, &fs) == 0) {
                return fail(core::ErrorCode::Internal, "PDFium could not place the text");
            }
            const int mode = static_cast<int>(FPDFTextObj_GetTextRenderMode(object));
            FPDFPageObj_SetFillColor(object, red, green, blue, 255);
            if (mode == 1 || mode == 2) {
                FPDFPageObj_SetStrokeColor(object, red, green, blue, 255);
            }
            memberUsed[k] = true;
            context.tagOf[object] = block.tag;
            continue;
        }

        OwnedObject object(FPDFPageObj_CreateTextObj(context.document, chosen.font, static_cast<float>(size)));
        if (object.get() == nullptr) {
            return fail(core::ErrorCode::Internal, "PDFium could not create a text object");
        }
        if (FPDFText_SetText(object.get(), utf16.data()) == 0) {
            return fail(core::ErrorCode::Internal, "PDFium could not set the text");
        }
        const FS_MATRIX fs =
            toFs(core::Matrix{block.placement.a, block.placement.b, block.placement.c, block.placement.d,
                              origin.x, origin.y});
        if (FPDFPageObj_SetMatrix(object.get(), &fs) == 0) {
            return fail(core::ErrorCode::Internal, "PDFium could not place the text");
        }
        if (renderMode != 0) {
            FPDFTextObj_SetTextRenderMode(object.get(), static_cast<FPDF_TEXT_RENDERMODE>(renderMode));
        }
        FPDFPageObj_SetFillColor(object.get(), red, green, blue, 255);
        if (renderMode == 1 || renderMode == 2) {
            FPDFPageObj_SetStrokeColor(object.get(), red, green, blue, 255);
        }
        const FPDF_PAGEOBJECT handle = object.release();
        FPDFPage_InsertObject(context.page, handle);
        context.originOf[handle] = PdfContentOrigin{PdfContentOrigin::Kind::Created, 0, block.tag};
        context.tagOf[handle] = block.tag;
    }
    for (std::size_t k = 0; k < members.size(); ++k) {
        if (!memberUsed[k]) {
            context.pendingRemoval.push_back(members[k]);
        }
    }
    return core::ok();
}

} // namespace

core::Result<ContentApplyResult> applyContentEdits(FPDF_DOCUMENT document,
                                                   FPDF_PAGE page,
                                                   const PdfPageContentEdits& edits,
                                                   FontCache& fonts) {
    const int count = FPDFPage_CountObjects(page);
    if (count < 0) {
        return fail(core::ErrorCode::InvalidDocument, "PDFium could not enumerate the page objects");
    }
    if (core::Status valid = validate(edits, static_cast<std::size_t>(count)); !valid.has_value()) {
        return std::unexpected(valid.error());
    }

    ApplyContext context;
    context.document = document;
    context.page = page;
    context.fonts = &fonts;
    for (int i = 0; i < count; ++i) {
        const FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, i);
        if (object == nullptr) {
            return fail(core::ErrorCode::InvalidDocument, "PDFium returned a null page object");
        }
        context.sources.push_back(object);
        context.types.push_back(FPDFPageObj_GetType(object));
        context.originOf[object] = PdfContentOrigin{PdfContentOrigin::Kind::Source, static_cast<std::uint32_t>(i), 0};
    }

    // Object edits in source order. Removals are deferred to the very end:
    // PDFium frees a font when its last text object dies, and the fonts of
    // removed objects may still be needed by the text blocks below.
    for (const PdfObjectEdit& edit : edits.objects) {
        const std::size_t index = edit.sourceIndex;
        const FPDF_PAGEOBJECT object = context.sources[index];
        if (edit.remove) {
            context.pendingRemoval.push_back(object);
            continue;
        }
        const int type = context.types[index];
        core::Matrix frame;
        if (edit.replaceImage != nullptr) {
            if (type != kObjImage) {
                return fail(core::ErrorCode::InvalidArgument, "only image objects can be replaced");
            }
            FS_MATRIX current{};
            if (FPDFPageObj_GetMatrix(object, &current) == 0) {
                return fail(core::ErrorCode::InvalidDocument, "PDFium could not read the image matrix");
            }
            frame = fromFs(current);
            if (edit.transform.has_value()) {
                frame = *edit.transform * frame;
            }
            core::Matrix placed;
            if (core::Status replaced = replaceImage(context, index, *edit.replaceImage, frame, object, placed);
                !replaced.has_value()) {
                return std::unexpected(replaced.error());
            }
            const FS_MATRIX fs = toFs(placed);
            if (FPDFPageObj_SetMatrix(object, &fs) == 0) {
                return fail(core::ErrorCode::Internal, "PDFium could not place the replacement image");
            }
        } else if (edit.transform.has_value()) {
            const FS_MATRIX fs = toFs(*edit.transform);
            if (FPDFPageObj_TransformF(object, &fs) == 0) {
                return fail(core::ErrorCode::Internal, "PDFium could not transform an object");
            }
        } else {
            continue;
        }
        if (edit.transform.has_value()) {
            // A clipped object moves together with its clip.
            if (const FPDF_CLIPPATH clip = FPDFPageObj_GetClipPath(object);
                clip != nullptr && FPDFClipPath_CountPaths(clip) > 0) {
                const core::Matrix& t = *edit.transform;
                FPDFPageObj_TransformClipPath(object, t.a, t.b, t.c, t.d, t.tx, t.ty);
            }
        }
    }

    for (const PdfTextBlockEdit& block : edits.textBlocks) {
        if (core::Status applied = applyTextBlock(context, block); !applied.has_value()) {
            return std::unexpected(applied.error());
        }
    }

    for (const FPDF_PAGEOBJECT object : context.pendingRemoval) {
        if (FPDFPage_RemoveObject(page, object) == 0) {
            return fail(core::ErrorCode::Internal, "PDFium could not remove an object");
        }
        context.originOf.erase(object);
        context.tagOf.erase(object);
        FPDFPageObj_Destroy(object);
    }

    if (!edits.empty() && FPDFPage_GenerateContent(page) == 0) {
        return fail(core::ErrorCode::Internal, "PDFium could not regenerate the page content");
    }

    ContentApplyResult result;
    const int finalCount = FPDFPage_CountObjects(page);
    if (finalCount < 0) {
        return fail(core::ErrorCode::Internal, "PDFium could not enumerate the edited page objects");
    }
    for (int i = 0; i < finalCount; ++i) {
        const FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, i);
        const auto origin = context.originOf.find(object);
        if (origin == context.originOf.end()) {
            return fail(core::ErrorCode::Internal, "an edited page object has no origin");
        }
        result.origins.push_back(origin->second);
        const auto tag = context.tagOf.find(object);
        result.blockTags.push_back(tag == context.tagOf.end() ? 0 : tag->second);
    }
    result.substitutedTags = std::move(context.substituted);
    return result;
}

// --- Materialization -------------------------------------------------------------------

MaterializedPage::~MaterializedPage() {
    if (page_ != nullptr) {
        FPDF_ClosePage(page_);
        page_ = nullptr;
    }
    if (document_ != nullptr) {
        FPDF_CloseDocument(document_);
        document_ = nullptr;
    }
}

core::Result<std::unique_ptr<MaterializedPage>> materializePage(FPDF_DOCUMENT source,
                                                                int pageIndex,
                                                                const PdfPageContentEdits& edits) {
    try {
        // The source page's own view and annotation count, to verify the
        // import keeps them.
        std::optional<PdfPageView> sourceView;
        int sourceAnnots = 0;
        {
            ScopedPage sourcePage(FPDF_LoadPage(source, pageIndex));
            if (sourcePage.get() == nullptr) {
                return fail(core::ErrorCode::InvalidDocument, "PDFium could not load the source page");
            }
            sourceView = nativePageView(sourcePage.get());
            sourceAnnots = std::max(0, FPDFPage_GetAnnotCount(sourcePage.get()));
        }

        auto result = std::unique_ptr<MaterializedPage>(new MaterializedPage());
        {
            ScopedDocument scratch(FPDF_CreateNewDocument());
            if (scratch.get() == nullptr) {
                return fail(core::ErrorCode::OutOfMemory, "PDFium could not create a scratch document");
            }
            const int indices[1] = {pageIndex};
            if (FPDF_ImportPagesByIndex(scratch.get(), source, indices, 1, 0) == 0) {
                return fail(core::ErrorCode::InvalidDocument, "PDFium could not import the page");
            }
            {
                ScopedPage page(FPDF_LoadPage(scratch.get(), 0));
                if (page.get() == nullptr) {
                    return fail(core::ErrorCode::InvalidDocument, "PDFium could not load the imported page");
                }
                const std::optional<PdfPageView> importedView = nativePageView(page.get());
                if (sourceView != importedView || std::max(0, FPDFPage_GetAnnotCount(page.get())) != sourceAnnots) {
                    return fail(core::ErrorCode::Internal,
                                "the imported page does not keep its view and annotations");
                }
                FontCache fonts(scratch.get());
                auto applied = applyContentEdits(scratch.get(), page.get(), edits, fonts);
                if (!applied.has_value()) {
                    return std::unexpected(applied.error());
                }
                result->applied_ = std::move(*applied);
                if (core::Status saved = saveToMemory(scratch.get(), result->buffer_); !saved.has_value()) {
                    return std::unexpected(saved.error());
                }
            }
        }

        result->document_ = FPDF_LoadMemDocument64(result->buffer_.data(), result->buffer_.size(), nullptr);
        if (result->document_ == nullptr) {
            return fail(core::ErrorCode::Internal, "PDFium could not reload the edited page");
        }
        result->page_ = FPDF_LoadPage(result->document_, 0);
        if (result->page_ == nullptr) {
            return fail(core::ErrorCode::Internal, "PDFium could not load the edited page");
        }
        const int count = FPDFPage_CountObjects(result->page_);
        if (count < 0 || static_cast<std::size_t>(count) != result->applied_.origins.size()) {
            return fail(core::ErrorCode::Internal, "the edited page has an unexpected number of objects");
        }
        return result;
    } catch (const std::bad_alloc&) {
        return fail(core::ErrorCode::OutOfMemory, "out of memory while applying content edits");
    } catch (const std::exception&) {
        return fail(core::ErrorCode::Internal, "unexpected failure while applying content edits");
    }
}

// --- ContentState ------------------------------------------------------------------------

void ContentState::closeAll() {
    entries_.clear(); // closes pages and documents
    entryBytes_ = 0;
    source_.clear();
    sourceOrder_.clear();
    if (reader_ != nullptr) {
        FPDF_CloseDocument(reader_);
        reader_ = nullptr;
    }
    if (probeReference_ != nullptr) {
        FPDF_CloseDocument(probeReference_);
        probeReference_ = nullptr;
    }
}

core::Result<PdfPageContentPtr> ContentState::sourcePage(std::size_t pageIndex) {
    if (const auto cached = source_.find(pageIndex); cached != source_.end()) {
        return cached->second;
    }
    if (reader_ == nullptr) {
        return fail(core::ErrorCode::Internal, "no content reader");
    }
    PdfPageContent content;
    {
        ScopedPage page(FPDF_LoadPage(reader_, static_cast<int>(pageIndex)));
        if (page.get() == nullptr) {
            return fail(core::ErrorCode::InvalidDocument, "PDFium could not load the page");
        }
        auto extracted = extractPageContent(page.get());
        if (!extracted.has_value()) {
            return std::unexpected(extracted.error());
        }
        content = std::move(*extracted);
    }
    if (content.truncated) {
        content.regenerationSafe = false;
        content.regenerationIssue = "page has too many objects";
    } else {
        const RegenerationProbe probe = probeRegeneration(reader_, probeReference_, static_cast<int>(pageIndex));
        content.regenerationSafe = probe.safe;
        content.regenerationIssue = probe.issue;
    }
    PdfPageContentPtr shared = std::make_shared<const PdfPageContent>(std::move(content));
    if (source_.size() >= kSourceCacheCapacity && !sourceOrder_.empty()) {
        source_.erase(sourceOrder_.front());
        sourceOrder_.pop_front();
    }
    source_.emplace(pageIndex, shared);
    sourceOrder_.push_back(pageIndex);
    return shared;
}

core::Result<ContentState::Entry*> ContentState::materialized(std::size_t pageIndex,
                                                               const PdfPageContentEditsPtr& edits) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->alive.expired()) {
            entryBytes_ -= std::min(entryBytes_, it->page->byteSize());
            it = entries_.erase(it);
            continue;
        }
        if (it->pageIndex == pageIndex && it->key == edits.get()) {
            entries_.splice(entries_.begin(), entries_, it);
            return &entries_.front();
        }
        ++it;
    }

    auto source = sourcePage(pageIndex);
    if (!source.has_value()) {
        return std::unexpected(source.error());
    }
    if (!(*source)->regenerationSafe) {
        return fail(core::ErrorCode::InvalidArgument,
                    "the page content cannot be edited safely: " + (*source)->regenerationIssue);
    }
    auto page = materializePage(reader_, static_cast<int>(pageIndex), *edits);
    if (!page.has_value()) {
        return std::unexpected(page.error());
    }
    Entry entry;
    entry.pageIndex = pageIndex;
    entry.key = edits.get();
    entry.alive = edits;
    entry.page = std::move(*page);
    entryBytes_ += entry.page->byteSize();
    entries_.push_front(std::move(entry));
    while (entries_.size() > 1 &&
           (entries_.size() > kMaterializedCapacity || entryBytes_ > kMaterializedByteBudget)) {
        entryBytes_ -= std::min(entryBytes_, entries_.back().page->byteSize());
        entries_.pop_back();
    }
    return &entries_.front();
}

core::Result<PdfPageContentPtr> ContentState::editedContent(Entry& entry) {
    if (entry.content != nullptr) {
        return entry.content;
    }
    auto extracted = extractPageContent(entry.page->page());
    if (!extracted.has_value()) {
        return std::unexpected(extracted.error());
    }
    PdfPageContent content = std::move(*extracted);
    const ContentApplyResult& applied = entry.page->applied();
    if (content.objects.size() != applied.origins.size()) {
        return fail(core::ErrorCode::Internal, "the edited page has an unexpected number of objects");
    }
    for (std::size_t i = 0; i < content.objects.size(); ++i) {
        PdfContentObject& object = content.objects[i];
        object.origin = applied.origins[i];
        object.blockTag = applied.blockTags[i];
        object.fontSubstituted = object.blockTag != 0 && applied.substitutedTags.count(object.blockTag) != 0;
    }
    auto source = sourcePage(entry.pageIndex);
    if (source.has_value()) {
        content.regenerationSafe = (*source)->regenerationSafe;
        content.regenerationIssue = (*source)->regenerationIssue;
    }
    entry.content = std::make_shared<const PdfPageContent>(std::move(content));
    return entry.content;
}

} // namespace rivet::pdf::internal
