// SPDX-License-Identifier: MPL-2.0

#include "pdf/PdfBuild.hpp"

#include "pdf/BundledFonts.hpp"
#include "pdf/PdfTextLayout.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace rivet::pdf {

namespace {

core::Status invalid(std::string message) {
    return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument, std::move(message), "pdf"));
}

// "page 3 item 7: " (indices are Rivet-generated, never document content).
std::string where(std::size_t page, const char* kind, std::size_t index) {
    return "page " + std::to_string(page) + " " + kind + " " + std::to_string(index) + ": ";
}

std::string codePointName(char32_t cp) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "U+%04X", static_cast<unsigned>(cp));
    return buffer;
}

bool validColor(const PdfColor& c) {
    const auto ok = [](float v) { return std::isfinite(v) && v >= 0.0F && v <= 1.0F; };
    return ok(c.r) && ok(c.g) && ok(c.b);
}

bool boxOnPage(const PdfBox& box, const PdfBuildPage& page) {
    return box.isValid() && PdfBox{0.0, 0.0, page.width, page.height}.contains(box, kBuildPageEpsilon);
}

bool pointOnPage(const PdfPoint& p, const PdfBuildPage& page) {
    return std::isfinite(p.x) && std::isfinite(p.y) && p.x >= -kBuildPageEpsilon && p.y >= -kBuildPageEpsilon &&
           p.x <= page.width + kBuildPageEpsilon && p.y <= page.height + kBuildPageEpsilon;
}

core::Status validateRect(const PdfBuildRect& rect, const PdfBuildPage& page, const std::string& at) {
    if (!boxOnPage(rect.box, page)) return invalid(at + "rectangle is empty, not finite or outside the page");
    if (!rect.fill.has_value() && !rect.stroke.has_value()) {
        return invalid(at + "rectangle has neither fill nor stroke");
    }
    if (rect.fill.has_value() && !validColor(*rect.fill)) return invalid(at + "fill color out of range");
    if (rect.stroke.has_value()) {
        if (!validColor(*rect.stroke)) return invalid(at + "stroke color out of range");
        const double width = static_cast<double>(rect.strokeWidth);
        if (!std::isfinite(width) || width <= 0.0 || width > kMaxBuildStrokeWidth) {
            return invalid(at + "stroke width out of range");
        }
    }
    return core::ok();
}

core::Status validateText(const PdfBuildText& text, const PdfBuildPage& page, const std::string& at) {
    if (static_cast<std::size_t>(text.font) > static_cast<std::size_t>(PdfBundledFont::MonoBold)) {
        return invalid(at + "unknown bundled font");
    }
    if (!std::isfinite(text.fontSize) || text.fontSize < kMinFontSize || text.fontSize > kMaxFontSize) {
        return invalid(at + "font size out of range");
    }
    if (!validColor(text.color)) return invalid(at + "text color out of range");
    if (!pointOnPage(text.origin, page)) return invalid(at + "text origin is not finite or outside the page");
    if (text.utf8.empty()) return invalid(at + "text run is empty");
    if (text.utf8.size() > kMaxBuildTextBytes) return invalid(at + "text run is too long");
    bool hadInvalid = false;
    const std::u32string decoded = decodeUtf8(text.utf8, &hadInvalid);
    if (hadInvalid) return invalid(at + "text run is not valid UTF-8");
    for (const char32_t cp : decoded) {
        // One run is one line: line breaks, tabs and other controls have no
        // glyph (bundledFontCovers accepts them for the layout's sake).
        const bool control = cp < 0x20 || (cp >= 0x7F && cp < 0xA0);
        if (control || !bundledFontCovers(text.font, cp)) {
            return invalid(at + "text contains " + codePointName(cp) + ", which the font cannot show");
        }
    }
    return core::ok();
}

core::Status validateImage(const PdfBuildImage& image, const PdfBuildPage& page, const std::string& at) {
    if (!boxOnPage(image.box, page)) return invalid(at + "image box is empty, not finite or outside the page");
    if (core::Status status = validateImageData(image.image); !status) {
        return invalid(at + "invalid image data");
    }
    return core::ok();
}

char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

core::Status validateUri(std::string_view uri, const std::string& at) {
    if (uri.empty() || uri.size() > kMaxBuildUriBytes) return invalid(at + "link URI is empty or too long");
    for (const char c : uri) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte >= 0x7F) {
            return invalid(at + "link URI contains a control, space or non-ASCII character");
        }
    }
    const std::size_t colon = uri.find(':');
    if (colon == std::string_view::npos || colon == 0) return invalid(at + "link URI has no scheme");
    std::string scheme;
    for (const char c : uri.substr(0, colon)) scheme += lower(c);
    const std::string_view rest = uri.substr(colon + 1);
    if (scheme == "http" || scheme == "https") {
        if (rest.size() <= 2 || rest.substr(0, 2) != "//") return invalid(at + "link URI has no authority");
        return core::ok();
    }
    if (scheme == "mailto") {
        if (rest.empty()) return invalid(at + "link URI has no address");
        return core::ok();
    }
    return invalid(at + "link URI scheme is not allowed");
}

} // namespace

core::Status validatePdfBuild(const PdfBuildDocument& document) {
    if (document.pages.empty()) return invalid("the document has no pages");
    if (document.pages.size() > kMaxBuildPages) return invalid("the document has too many pages");
    std::size_t totalItems = 0;
    for (std::size_t p = 0; p < document.pages.size(); ++p) {
        const PdfBuildPage& page = document.pages[p];
        const std::string pageAt = "page " + std::to_string(p) + ": ";
        const auto sizeOk = [](double v) {
            return std::isfinite(v) && v >= kMinBuildPageSize && v <= kMaxBuildPageSize;
        };
        if (!sizeOk(page.width) || !sizeOk(page.height)) return invalid(pageAt + "page size out of range");
        if (page.items.size() > kMaxBuildItemsPerPage) return invalid(pageAt + "too many items");
        if (page.links.size() > kMaxBuildLinksPerPage) return invalid(pageAt + "too many links");
        totalItems += page.items.size();
        if (totalItems > kMaxBuildItemsTotal) return invalid("the document has too many items");
        for (std::size_t i = 0; i < page.items.size(); ++i) {
            const std::string at = where(p, "item", i);
            const core::Status status = std::visit(
                [&](const auto& item) -> core::Status {
                    using T = std::decay_t<decltype(item)>;
                    if constexpr (std::is_same_v<T, PdfBuildRect>) {
                        return validateRect(item, page, at);
                    } else if constexpr (std::is_same_v<T, PdfBuildText>) {
                        return validateText(item, page, at);
                    } else {
                        return validateImage(item, page, at);
                    }
                },
                page.items[i]);
            if (!status) return status;
        }
        for (std::size_t l = 0; l < page.links.size(); ++l) {
            const std::string at = where(p, "link", l);
            if (!boxOnPage(page.links[l].box, page)) {
                return invalid(at + "link box is empty, not finite or outside the page");
            }
            if (core::Status status = validateUri(page.links[l].uri, at); !status) return status;
        }
    }
    return core::ok();
}

} // namespace rivet::pdf
