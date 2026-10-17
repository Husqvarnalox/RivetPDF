// SPDX-License-Identifier: MPL-2.0
#include "pdf/sign/PdfSigner.hpp"

#include "core/io/AtomicFileWriter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>

namespace rivet::pdf::sign {

namespace {

constexpr std::size_t kMaxAttempts = 3;
constexpr std::size_t kDefaultReserveBase = 3072;
constexpr std::size_t kMaxReserve = std::size_t{4} << 20;

PdfObject real(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s.empty() || s == "-0") s = "0";
    return PdfObject::makeReal(std::move(s));
}

PdfObject name(std::string_view n) { return PdfObject::makeName(std::string(n)); }
PdfObject integer(std::int64_t v) { return PdfObject::makeInt(v); }
PdfObject ref(std::uint32_t num, std::uint32_t gen = 0) { return PdfObject::makeRef({num, gen}); }

struct CivilTime {
    int year, month, day, hour, minute, second;
};

CivilTime civil(std::chrono::system_clock::time_point t) {
    using namespace std::chrono;
    const auto secs = floor<seconds>(t);
    const auto days = floor<std::chrono::days>(secs);
    const year_month_day ymd{days};
    const auto tod = secs - days;
    const hh_mm_ss<seconds> hms{tod};
    return {static_cast<int>(ymd.year()),
            static_cast<int>(static_cast<unsigned>(ymd.month())),
            static_cast<int>(static_cast<unsigned>(ymd.day())),
            static_cast<int>(hms.hours().count()),
            static_cast<int>(hms.minutes().count()),
            static_cast<int>(hms.seconds().count())};
}

std::string pdfDate(std::chrono::system_clock::time_point t) {
    const CivilTime c = civil(t);
    char buf[40];
    std::snprintf(buf, sizeof buf, "D:%04d%02d%02d%02d%02d%02d+00'00'", c.year, c.month, c.day, c.hour, c.minute,
                  c.second);
    return buf;
}

std::string humanDate(std::chrono::system_clock::time_point t) {
    const CivilTime c = civil(t);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d UTC", c.year, c.month, c.day, c.hour, c.minute,
                  c.second);
    return buf;
}

std::string_view digestPdfName(crypto::DigestAlgorithm d) {
    switch (d) {
    case crypto::DigestAlgorithm::Sha384: return "SHA384";
    case crypto::DigestAlgorithm::Sha512: return "SHA512";
    default: return "SHA256";
    }
}

std::string latin1Only(std::string_view utf8) {
    std::string out;
    for (std::size_t i = 0; i < utf8.size();) {
        const auto c = static_cast<std::uint8_t>(utf8[i]);
        if (c < 0x80) {
            out.push_back(c < 0x20 ? ' ' : static_cast<char>(c));
            ++i;
        } else {
            std::size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
            if (c == 0xC2 && i + 1 < utf8.size() && static_cast<std::uint8_t>(utf8[i + 1]) >= 0xA0) {
                out.push_back(static_cast<char>(utf8[i + 1])); // U+00A0..U+00BF
            } else if (c == 0xC3 && i + 1 < utf8.size()) {
                out.push_back(static_cast<char>(static_cast<std::uint8_t>(utf8[i + 1]) + 0x40)); // U+00C0..U+00FF
            } else {
                out.push_back('?');
            }
            i += len;
        }
    }
    return out;
}

// One object of the new revision whose body is already serialized.
struct PlannedObject {
    std::uint32_t num = 0;
    std::uint32_t gen = 0;
    std::string head;       // serialized object (dictionary/array); a stream's dictionary
    std::string streamData; // non-empty or `stream` set: a stream object
    bool stream = false;
};

std::string lockActionName(FieldLockAction a) {
    switch (a) {
    case FieldLockAction::All: return "All";
    case FieldLockAction::Include: return "Include";
    case FieldLockAction::Exclude: return "Exclude";
    case FieldLockAction::None: break;
    }
    return "All";
}

PdfObject lockDict(const FieldLock& lock, bool forTransform) {
    PdfDict d;
    d.set("Type", name(forTransform ? "TransformParams" : "SigFieldLock"));
    d.set("Action", name(lockActionName(lock.action)));
    if (lock.action != FieldLockAction::All) {
        PdfArray names;
        for (const std::string& f : lock.fields) names.push_back(PdfObject::makeTextString(f));
        d.set("Fields", PdfObject::makeArray(std::move(names)));
    }
    if (forTransform) d.set("V", name("1.2"));
    return PdfObject::makeDict(std::move(d));
}

struct Plan {
    std::vector<PlannedObject> objects;
    std::uint32_t sigNum = 0;
    std::uint32_t xrefNum = 0; // 0: classic table
    std::uint32_t size = 0;    // trailer /Size
    std::uint64_t prev = 0;    // previous startxref
    PdfDict trailerKeys;       // /Root, /Info, /ID copied from the previous trailer
    PdfRef catalogRef;
    std::string fieldName;
    bool pades = false;
    bool certify = false;
    bool docTs = false; // /Type /DocTimeStamp, /SubFilter /ETSI.RFC3161
};

SignOk requireNotStream(PdfFile& f, std::uint32_t num) {
    const auto e = f.lookup(num);
    if (!e || e->type != XrefEntry::Type::InUse) return {};
    auto ind = f.loadIndirect(num);
    if (ind && ind->isStream) return fail(SignErrorCode::UnsupportedStructure, "a structural object is a stream");
    return {};
}

std::uint32_t genOf(PdfFile& f, std::uint32_t num) {
    const auto e = f.lookup(num);
    return e && e->type == XrefEntry::Type::InUse ? e->b : 0;
}

PlannedObject dictObject(std::uint32_t num, std::uint32_t gen, const PdfDict& d) {
    PlannedObject o;
    o.num = num;
    o.gen = gen;
    o.head = serialize(PdfObject::makeDict(d));
    return o;
}

std::string appearanceStream(double w, double h, const std::vector<std::string>& lines) {
    const double fontSize = std::clamp((h - 8.0) / (static_cast<double>(std::max<std::size_t>(lines.size(), 1)) * 1.25),
                                       4.0, 10.0);
    std::string s = "q\n1 g\n0 0 " + serialize(real(w)) + " " + serialize(real(h)) + " re f\n0 G 0.5 w\n0.25 0.25 " +
                    serialize(real(w - 0.5)) + " " + serialize(real(h - 0.5)) + " re S\n0 g\nBT\n/F1 " +
                    serialize(real(fontSize)) + " Tf\n";
    double y = h - 4.0 - fontSize;
    for (const std::string& line : lines) {
        s += "1 0 0 1 4 " + serialize(real(y)) + " Tm\n";
        std::string lit;
        appendLiteralString(lit, latin1Only(line));
        s += lit + " Tj\n";
        y -= fontSize * 1.25;
    }
    s += "ET\nQ\n";
    return s;
}

// `identity` is only needed for the text of a visible appearance (null for a document time-stamp).
SignResult<Plan> makePlan(PdfFile& f, const SignRequest& req, crypto::SigningIdentity* identity, bool docTs = false) {
    Plan plan;
    plan.docTs = docTs;
    plan.pades = docTs || req.profile == SignatureProfile::PadesBaselineB;
    plan.certify = req.certification != DocMdpLevel::None;
    if (f.isEncrypted())
        return fail(SignErrorCode::EncryptedDocument,
                    "the document is encrypted; signing encrypted PDFs is not supported (ADR-0037)");

    auto cat = f.catalog();
    if (!cat) return std::unexpected(cat.error());
    plan.catalogRef = cat->first;
    PdfDict catalog = cat->second;
    if (auto r = requireNotStream(f, cat->first.num); !r) return std::unexpected(r.error());

    // ---- existing signatures and certification state
    auto fields = f.fields();
    if (!fields) return std::unexpected(fields.error());
    bool priorSignature = false;
    std::set<std::string> usedNames;
    for (const FieldInfo& fi : *fields) {
        usedNames.insert(fi.fullName);
        if (fi.fieldType == "Sig") {
            const PdfObject* v = fi.dict.find("V");
            if (v && !v->isNull()) priorSignature = true;
        }
    }
    bool certPresent = false;
    std::optional<DocMdpLevel> certLevel;
    if (const PdfObject* perms = catalog.find("Perms")) {
        const PdfObject p = f.resolve(*perms, 0);
        if (p.isDict()) {
            if (const PdfObject* dm = p.dict->find("DocMDP")) {
                const PdfObject sig = f.resolve(*dm, 0);
                if (sig.isDict()) {
                    certPresent = true;
                    if (const PdfObject* rr = sig.dict->find("Reference")) {
                        const PdfObject arr = f.resolve(*rr, 0);
                        if (arr.isArray())
                            for (const PdfObject& e : *arr.array) {
                                const PdfObject d = f.resolve(e, 0);
                                if (!d.isDict() || !d.dict->find("TransformMethod") ||
                                    !d.dict->find("TransformMethod")->isName("DocMDP"))
                                    continue;
                                const PdfObject* tp = d.dict->find("TransformParams");
                                const PdfObject params = tp ? f.resolve(*tp, 0) : PdfObject::null();
                                const auto pv = params.isDict() ? dictInt(*params.dict, "P") : std::nullopt;
                                certLevel = !pv || *pv == 2   ? DocMdpLevel::FormFill
                                            : *pv == 1        ? DocMdpLevel::NoChanges
                                            : *pv == 3        ? DocMdpLevel::Annotate
                                                              : DocMdpLevel::NoChanges; // unknown level: most restrictive
                            }
                    }
                }
            }
        }
    }
    // A document time-stamp is not a change to the content: it stays allowed under every DocMDP level.
    if (!docTs && certPresent && (!certLevel || *certLevel == DocMdpLevel::NoChanges))
        return fail(SignErrorCode::ViolatesCertification,
                    "the document is certified for no changes; a signature cannot be added");
    if (plan.certify && (priorSignature || certPresent))
        return fail(SignErrorCode::CertificationNotAllowed,
                    "a certification signature must be the first signature of the document");

    // ---- object numbers
    std::uint64_t next = f.nextObjectNumber();
    auto allocate = [&]() -> SignResult<std::uint32_t> {
        if (next >= 0x7FFFFFF0ull) return fail(SignErrorCode::LimitExceeded, "object numbers exhausted");
        return static_cast<std::uint32_t>(next++);
    };
    auto sigNum = allocate();
    if (!sigNum) return std::unexpected(sigNum.error());
    plan.sigNum = *sigNum;
    const PdfRef sigRef{plan.sigNum, 0};

    // ---- AcroForm
    PdfDict acro;
    std::optional<PdfRef> acroRef;
    bool acroDirect = false;
    if (const PdfObject* af = catalog.find("AcroForm")) {
        if (af->isRef()) {
            auto o = f.loadObject(af->ref.num);
            if (!o) return std::unexpected(o.error());
            if (!*o || !(*o)->isDict()) return fail(SignErrorCode::UnsupportedStructure, "/AcroForm is not a dictionary");
            acro = *(*o)->dict;
            acroRef = af->ref;
            if (auto r = requireNotStream(f, af->ref.num); !r) return std::unexpected(r.error());
        } else if (af->isDict()) {
            acro = *af->dict;
            acroDirect = true;
        } else if (!af->isNull()) {
            return fail(SignErrorCode::UnsupportedStructure, "/AcroForm is not a dictionary");
        }
    }
    acro.set("SigFlags", integer(3));

    PdfObject fieldNew; // widget / field reference to add to /Fields (new fields only)
    std::map<std::uint32_t, PlannedObject> modified; // by object number
    std::optional<PdfRef> widgetRef;

    const SignaturePlacement& pl = req.placement;
    if (pl.kind == SignaturePlacement::Kind::ExistingField) {
        const FieldInfo* target = nullptr;
        for (const FieldInfo& fi : *fields)
            if (fi.fullName == pl.fieldName && fi.fieldType == "Sig") target = &fi;
        if (!target) return fail(SignErrorCode::FieldNotFound, "no signature field with that name");
        const PdfObject* v = target->dict.find("V");
        if (v && !v->isNull()) return fail(SignErrorCode::FieldNotEmpty, "the signature field is already signed");
        if (!target->ref) return fail(SignErrorCode::UnsupportedStructure, "the signature field is a direct object");
        if (auto r = requireNotStream(f, target->ref->num); !r) return std::unexpected(r.error());
        PdfDict fd = target->dict;
        fd.set("V", PdfObject::makeRef(sigRef));
        if (req.fieldLock && req.fieldLock->action != FieldLockAction::None) fd.set("Lock", lockDict(*req.fieldLock, false));
        modified[target->ref->num] = dictObject(target->ref->num, genOf(f, target->ref->num), fd);
        plan.fieldName = target->fullName;
    } else {
        // New field: widget annotation + field in one dictionary.
        std::string fieldName = pl.fieldName;
        if (fieldName.empty()) {
            for (int i = 1; i < 100000; ++i) {
                fieldName = "Signature" + std::to_string(i);
                if (!usedNames.count(fieldName)) break;
            }
        } else if (usedNames.count(fieldName)) {
            return fail(SignErrorCode::InvalidArgument, "a field with that name already exists");
        }
        plan.fieldName = fieldName;

        auto page = f.findPage(pl.pageIndex);
        if (!page) return std::unexpected(page.error());
        auto widgetNum = allocate();
        if (!widgetNum) return std::unexpected(widgetNum.error());
        widgetRef = PdfRef{*widgetNum, 0};

        const bool visible = pl.kind == SignaturePlacement::Kind::NewVisible;
        PdfDict w;
        w.set("Type", name("Annot"));
        w.set("Subtype", name("Widget"));
        w.set("FT", name("Sig"));
        w.set("T", PdfObject::makeTextString(fieldName));
        w.set("V", PdfObject::makeRef(sigRef));
        w.set("F", integer(132)); // Print + Locked
        w.set("P", PdfObject::makeRef(page->ref));
        if (req.fieldLock && req.fieldLock->action != FieldLockAction::None) w.set("Lock", lockDict(*req.fieldLock, false));
        if (visible) {
            double x0 = std::min(pl.rect[0], pl.rect[2]), x1 = std::max(pl.rect[0], pl.rect[2]);
            double y0 = std::min(pl.rect[1], pl.rect[3]), y1 = std::max(pl.rect[1], pl.rect[3]);
            for (const double c : {x0, x1, y0, y1})
                if (!std::isfinite(c) || std::fabs(c) > 1e7)
                    return fail(SignErrorCode::InvalidArgument, "signature rectangle is not finite");
            if (x1 - x0 < 1.0 || y1 - y0 < 1.0)
                return fail(SignErrorCode::InvalidArgument, "signature rectangle is empty");
            w.set("Rect", PdfObject::makeArray({real(x0), real(y0), real(x1), real(y1)}));

            std::vector<std::string> lines = req.appearanceLines;
            if (lines.empty()) {
                const std::string who = !req.signerName.empty() ? req.signerName
                                        : !identity                 ? std::string("the signer")
                                        : identity->certificate().commonName ? *identity->certificate().commonName
                                                                            : identity->certificate().subject;
                lines.push_back("Digitally signed by " + who);
                lines.push_back("Date: " + humanDate(req.signingTime));
                if (!req.reason.empty()) lines.push_back("Reason: " + req.reason);
                if (!req.location.empty()) lines.push_back("Location: " + req.location);
            }
            auto apNum = allocate();
            if (!apNum) return std::unexpected(apNum.error());
            PdfDict apDict;
            apDict.set("Type", name("XObject"));
            apDict.set("Subtype", name("Form"));
            apDict.set("BBox", PdfObject::makeArray({integer(0), integer(0), real(x1 - x0), real(y1 - y0)}));
            PdfObject font = dictOf({{"Type", name("Font")},
                                     {"Subtype", name("Type1")},
                                     {"BaseFont", name("Helvetica")},
                                     {"Encoding", name("WinAnsiEncoding")}});
            apDict.set("Resources", dictOf({{"Font", dictOf({{"F1", font}})}}));
            PlannedObject ap;
            ap.num = *apNum;
            ap.stream = true;
            ap.streamData = appearanceStream(x1 - x0, y1 - y0, lines);
            apDict.set("Length", integer(static_cast<std::int64_t>(ap.streamData.size())));
            ap.head = serialize(PdfObject::makeDict(apDict));
            plan.objects.push_back(std::move(ap));
            w.set("AP", dictOf({{"N", ref(*apNum)}}));
        } else {
            w.set("Rect", PdfObject::makeArray({integer(0), integer(0), integer(0), integer(0)}));
        }
        plan.objects.push_back(dictObject(widgetRef->num, 0, w));

        // Page /Annots
        PdfDict pageDict = page->dict;
        if (auto r = requireNotStream(f, page->ref.num); !r) return std::unexpected(r.error());
        const PdfObject* annots = pageDict.find("Annots");
        if (!annots || annots->isNull()) {
            pageDict.set("Annots", PdfObject::makeArray({PdfObject::makeRef(*widgetRef)}));
            modified[page->ref.num] = dictObject(page->ref.num, genOf(f, page->ref.num), pageDict);
        } else if (annots->isArray()) {
            PdfArray items = *annots->array;
            items.push_back(PdfObject::makeRef(*widgetRef));
            pageDict.set("Annots", PdfObject::makeArray(std::move(items)));
            modified[page->ref.num] = dictObject(page->ref.num, genOf(f, page->ref.num), pageDict);
        } else if (annots->isRef()) {
            auto arrObj = f.loadObject(annots->ref.num);
            if (!arrObj) return std::unexpected(arrObj.error());
            if (!*arrObj || !(*arrObj)->isArray())
                return fail(SignErrorCode::UnsupportedStructure, "/Annots is not an array");
            if (auto r = requireNotStream(f, annots->ref.num); !r) return std::unexpected(r.error());
            PdfArray items = *(*arrObj)->array;
            items.push_back(PdfObject::makeRef(*widgetRef));
            PlannedObject o;
            o.num = annots->ref.num;
            o.gen = genOf(f, annots->ref.num);
            o.head = serialize(PdfObject::makeArray(std::move(items)));
            modified[o.num] = std::move(o);
        } else {
            return fail(SignErrorCode::UnsupportedStructure, "/Annots is not an array");
        }

        // AcroForm /Fields
        const PdfObject* fl = acro.find("Fields");
        if (!fl || fl->isNull()) {
            acro.set("Fields", PdfObject::makeArray({PdfObject::makeRef(*widgetRef)}));
        } else if (fl->isArray()) {
            PdfArray items = *fl->array;
            items.push_back(PdfObject::makeRef(*widgetRef));
            acro.set("Fields", PdfObject::makeArray(std::move(items)));
        } else if (fl->isRef()) {
            auto arrObj = f.loadObject(fl->ref.num);
            if (!arrObj) return std::unexpected(arrObj.error());
            if (!*arrObj || !(*arrObj)->isArray())
                return fail(SignErrorCode::UnsupportedStructure, "/AcroForm /Fields is not an array");
            if (auto r = requireNotStream(f, fl->ref.num); !r) return std::unexpected(r.error());
            PdfArray items = *(*arrObj)->array;
            items.push_back(PdfObject::makeRef(*widgetRef));
            PlannedObject o;
            o.num = fl->ref.num;
            o.gen = genOf(f, fl->ref.num);
            o.head = serialize(PdfObject::makeArray(std::move(items)));
            modified[o.num] = std::move(o);
        } else {
            return fail(SignErrorCode::UnsupportedStructure, "/AcroForm /Fields is not an array");
        }
    }

    // ---- catalog and AcroForm object
    bool catalogChanged = false;
    if (acroRef) {
        modified[acroRef->num] = dictObject(acroRef->num, genOf(f, acroRef->num), acro);
    } else {
        (void)acroDirect;
        catalog.set("AcroForm", PdfObject::makeDict(acro));
        catalogChanged = true;
    }
    if (plan.certify) {
        const PdfObject* perms = catalog.find("Perms");
        if (perms && perms->isRef())
            return fail(SignErrorCode::UnsupportedStructure, "an indirect /Perms dictionary is not supported");
        PdfDict p = perms && perms->isDict() ? *perms->dict : PdfDict{};
        p.set("DocMDP", PdfObject::makeRef(sigRef));
        catalog.set("Perms", PdfObject::makeDict(std::move(p)));
        catalogChanged = true;
    }
    if (plan.pades && (f.headerVersion().empty() || f.headerVersion()[0] != '2')) {
        // ETSI EN 319 142-1 / ISO 32000-1 developer extension for CAdES in PDF 1.x (ADR-0038).
        const PdfObject* ext = catalog.find("Extensions");
        const PdfObject esic = dictOf({{"BaseVersion", name("1.7")}, {"ExtensionLevel", integer(2)}});
        if (!ext || ext->isNull()) {
            catalog.set("Extensions", dictOf({{"ESIC", esic}}));
            catalogChanged = true;
        } else if (ext->isDict() && !ext->dict->find("ESIC")) {
            PdfDict e = *ext->dict;
            e.set("ESIC", esic);
            catalog.set("Extensions", PdfObject::makeDict(std::move(e)));
            catalogChanged = true;
        }
    }
    if (catalogChanged) modified[plan.catalogRef.num] = dictObject(plan.catalogRef.num, plan.catalogRef.gen, catalog);

    for (auto& [num, obj] : modified) plan.objects.push_back(std::move(obj));

    // ---- xref style and trailer
    const XrefSection& newest = f.section(0);
    if (newest.isStream) {
        auto xn = allocate();
        if (!xn) return std::unexpected(xn.error());
        plan.xrefNum = *xn;
    }
    plan.size = static_cast<std::uint32_t>(next);
    plan.prev = f.lastStartxref();
    const PdfDict trailer = f.mergedTrailer();
    const PdfObject* root = trailer.find("Root");
    if (!root || !root->isRef()) return fail(SignErrorCode::MalformedPdf, "trailer has no /Root");
    plan.trailerKeys.set("Root", *root);
    for (const char* key : {"Info", "ID"})
        if (const PdfObject* v = trailer.find(key)) plan.trailerKeys.set(key, *v);
    return plan;
}

struct Layout {
    std::string rev;               // appended bytes, including a leading EOL pad when needed
    std::size_t brField = 0;       // local offset of the first /ByteRange digit field
    std::size_t contentsStart = 0; // local offset of '<'
    std::size_t contentsEnd = 0;   // local offset just after '>'
};

void putField(std::string& rev, std::size_t at, std::uint64_t value) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%-10llu", static_cast<unsigned long long>(value));
    std::memcpy(rev.data() + at, buf, 10);
}

void appendObject(std::string& rev, const PlannedObject& o) {
    rev += std::to_string(o.num) + " " + std::to_string(o.gen) + " obj\n";
    rev += o.head;
    if (o.stream) {
        rev += "\nstream\n";
        rev += o.streamData;
        rev += "\nendstream";
    }
    rev += "\nendobj\n";
}

SignResult<Layout> layoutRevision(const Plan& plan, const SignRequest& req, std::uint64_t baseSize, bool needPad,
                                  std::size_t reserve, const std::optional<PdfRef>& dataRef) {
    Layout L;
    std::string& rev = L.rev;
    rev.reserve(8192 + reserve * 2);
    if (needPad) rev.push_back('\n');
    std::map<std::uint32_t, std::pair<std::uint64_t, std::uint32_t>> offsets; // num -> offset, gen

    for (const PlannedObject& o : plan.objects) {
        offsets[o.num] = {baseSize + rev.size(), o.gen};
        appendObject(rev, o);
    }

    // Signature dictionary with placeholders.
    offsets[plan.sigNum] = {baseSize + rev.size(), 0};
    rev += std::to_string(plan.sigNum) + " 0 obj\n<<\n/Type /" + (plan.docTs ? "DocTimeStamp" : "Sig") +
           "\n/Filter /Adobe.PPKLite\n/SubFilter ";
    rev += plan.docTs ? "/ETSI.RFC3161" : plan.pades ? "/ETSI.CAdES.detached" : "/adbe.pkcs7.detached";
    rev += "\n/ByteRange [0 ";
    L.brField = rev.size();
    rev += std::string(10, ' ') + " " + std::string(10, ' ') + " " + std::string(10, ' ') + "]\n/Contents ";
    L.contentsStart = rev.size();
    rev.push_back('<');
    rev.append(reserve * 2, '0');
    rev.push_back('>');
    L.contentsEnd = rev.size();
    if (plan.docTs) {
        rev += "\n>>\nendobj\n";
    } else {
    rev += "\n/M ";
    appendLiteralString(rev, pdfDate(req.signingTime));
    auto optionalText = [&](const char* key, const std::string& v) {
        if (v.empty()) return;
        rev += std::string("\n/") + key + " ";
        serialize(PdfObject::makeTextString(v), rev);
    };
    optionalText("Reason", req.reason);
    optionalText("Location", req.location);
    optionalText("Name", req.signerName);
    optionalText("ContactInfo", req.contactInfo);
    const bool lockRef = req.fieldLock && req.fieldLock->action != FieldLockAction::None;
    if ((plan.certify || lockRef) && dataRef) {
        const std::string data = " /Data " + std::to_string(dataRef->num) + " " + std::to_string(dataRef->gen) + " R";
        const std::string digestMethod = std::string(" /DigestMethod /") + std::string(digestPdfName(req.digest));
        rev += "\n/Reference [";
        if (plan.certify) {
            rev += "<< /Type /SigRef /TransformMethod /DocMDP" + digestMethod + data +
                   " /TransformParams << /Type /TransformParams /P " + std::to_string(static_cast<int>(req.certification)) +
                   " /V /1.2 >> >>";
        }
        if (lockRef) {
            rev += "<< /Type /SigRef /TransformMethod /FieldMDP" + digestMethod + data + " /TransformParams " +
                   serialize(lockDict(*req.fieldLock, true)) + " >>";
        }
        rev += "]";
    }
    rev += "\n>>\nendobj\n";
    }

    // Cross-reference section.
    const std::uint64_t xrefOffset = baseSize + rev.size();
    if (plan.xrefNum == 0) {
        rev += "xref\n";
        std::vector<std::pair<std::uint32_t, std::pair<std::uint64_t, std::uint32_t>>> all(offsets.begin(), offsets.end());
        std::size_t i = 0;
        while (i < all.size()) {
            std::size_t j = i + 1;
            while (j < all.size() && all[j].first == all[j - 1].first + 1) ++j;
            rev += std::to_string(all[i].first) + " " + std::to_string(j - i) + "\n";
            for (std::size_t k = i; k < j; ++k) {
                char line[32];
                std::snprintf(line, sizeof line, "%010llu %05u n \n", static_cast<unsigned long long>(all[k].second.first),
                              all[k].second.second);
                rev += line;
            }
            i = j;
        }
        PdfDict tr = plan.trailerKeys;
        tr.set("Size", PdfObject::makeInt(plan.size));
        tr.set("Prev", PdfObject::makeInt(static_cast<std::int64_t>(plan.prev)));
        rev += "trailer\n" + serialize(PdfObject::makeDict(tr)) + "\n";
    } else {
        offsets[plan.xrefNum] = {xrefOffset, 0};
        std::string data;
        PdfArray index;
        std::vector<std::pair<std::uint32_t, std::pair<std::uint64_t, std::uint32_t>>> all(offsets.begin(), offsets.end());
        std::size_t i = 0;
        while (i < all.size()) {
            std::size_t j = i + 1;
            while (j < all.size() && all[j].first == all[j - 1].first + 1) ++j;
            index.push_back(PdfObject::makeInt(all[i].first));
            index.push_back(PdfObject::makeInt(static_cast<std::int64_t>(j - i)));
            for (std::size_t k = i; k < j; ++k) {
                const std::uint64_t off = all[k].second.first;
                data.push_back(1);
                for (int sh = 24; sh >= 0; sh -= 8) data.push_back(static_cast<char>((off >> sh) & 0xFF));
                data.push_back(static_cast<char>((all[k].second.second >> 8) & 0xFF));
                data.push_back(static_cast<char>(all[k].second.second & 0xFF));
            }
            i = j;
        }
        PdfDict xd = plan.trailerKeys;
        xd.set("Type", name("XRef"));
        xd.set("Size", PdfObject::makeInt(plan.size));
        xd.set("Prev", PdfObject::makeInt(static_cast<std::int64_t>(plan.prev)));
        xd.set("W", PdfObject::makeArray({integer(1), integer(4), integer(2)}));
        xd.set("Index", PdfObject::makeArray(std::move(index)));
        xd.set("Length", integer(static_cast<std::int64_t>(data.size())));
        rev += std::to_string(plan.xrefNum) + " 0 obj\n" + serialize(PdfObject::makeDict(xd)) + "\nstream\n";
        rev += data;
        rev += "\nendstream\nendobj\n";
    }
    rev += "startxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";

    if (baseSize + rev.size() > kMaxSignableFileSize)
        return fail(SignErrorCode::LimitExceeded, "the signed file would exceed 2 GiB (ByteRange limit)");

    putField(rev, L.brField, baseSize + L.contentsStart);
    putField(rev, L.brField + 11, baseSize + L.contentsEnd);
    putField(rev, L.brField + 22, rev.size() - L.contentsEnd);
    return L;
}

} // namespace

namespace {

struct TimestampOutcome {
    bool attached = false;
    std::string skippedReason;
    std::size_t tokenBytes = 0;
};

std::string joinNotes(const std::vector<std::string>& notes) {
    std::string out;
    for (const std::string& n : notes) {
        if (!out.empty()) out += "; ";
        out += n;
    }
    return out;
}

// Requests the signature time-stamp for signer 0 and attaches it. A TSA
// failure is either fatal (TimestampFailed) or, when the request allows it,
// reported through `skippedReason` with the CMS left untouched.
SignResult<TimestampOutcome> applySignatureTimestamp(crypto::CmsSignedData& cms, const SignatureTimestampOptions& opt,
                                                     crypto::CryptoProvider& provider, const crypto::CancelToken& cancel) {
    if (cms.signers.empty()) return fail(SignErrorCode::Internal, "the CMS has no signer to time-stamp");
    auto imprint = crypto::signatureTimestampImprint(provider, cms.signers[0], opt.digest);
    if (!imprint) return failCrypto(imprint.error());
    auto fetched = opt.fetch(opt.digest, *imprint, cancel);
    std::string why;
    std::optional<crypto::CryptoErrorCode> code;
    if (!fetched) {
        if (fetched.error().code == crypto::CryptoErrorCode::Cancelled) return failCrypto(fetched.error());
        why = "the time-stamp request failed: " + std::string(crypto::toString(fetched.error().code)) + ": " +
              fetched.error().message;
        code = fetched.error().code;
    } else if (!fetched->ok()) {
        why = "the time-stamp token was rejected (" + std::string(crypto::toString(fetched->failure)) + ")";
        const std::string notes = joinNotes(fetched->validation.notes);
        if (!notes.empty()) why += ": " + notes;
    } else if (auto attached = crypto::attachSignatureTimestamp(cms, 0, fetched->tokenDer, provider); !attached) {
        why = "the time-stamp token could not be attached: " + attached.error().message;
        code = attached.error().code;
    }
    TimestampOutcome out;
    if (why.empty()) {
        out.attached = true;
        out.tokenBytes = fetched->tokenDer.size();
        return out;
    }
    if (opt.allowSignWithoutTimestamp) {
        out.skippedReason = std::move(why);
        return out;
    }
    return std::unexpected<SignError>(SignError{SignErrorCode::TimestampFailed, std::move(why), code});
}

std::size_t tokenReserve(std::size_t expected) { return expected != 0 ? expected : kDefaultTimestampTokenReserve; }

void fillContents(Layout& L, const Bytes& der) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    char* hex = L.rev.data() + L.contentsStart + 1;
    for (std::size_t i = 0; i < der.size(); ++i) {
        hex[2 * i] = kHex[der[i] >> 4];
        hex[2 * i + 1] = kHex[der[i] & 0x0F];
    }
}

} // namespace

std::string_view toString(PadesLevel level) {
    switch (level) {
    case PadesLevel::NotPades: return "not PAdES";
    case PadesLevel::BaselineB: return "PAdES B-B";
    case PadesLevel::BaselineT: return "PAdES B-T";
    }
    return "?";
}

SignResult<SignedPdf> signPdf(Bytes pdf, const SignRequest& req, crypto::CryptoProvider& provider,
                              crypto::SigningIdentity& identity) {
    if (req.cancel.cancelled()) return fail(SignErrorCode::Cancelled, "cancelled");
    if (auto ok = crypto::checkCreationAllowed(req.digest); !ok) return failCrypto(ok.error());
    if (req.timestamp.fetch)
        if (auto ok = crypto::checkCreationAllowed(req.timestamp.digest); !ok) return failCrypto(ok.error());
    if (pdf.size() > kMaxSignableFileSize - (std::uint64_t{16} << 20))
        return fail(SignErrorCode::LimitExceeded, "file too large to sign (limit 2 GiB)");

    const std::uint64_t baseSize = pdf.size();
    const bool needPad = pdf.empty() || (pdf.back() != '\n' && pdf.back() != '\r');
    Plan plan;
    std::optional<PdfRef> dataRef;
    {
        auto opened = PdfFile::open(pdf);
        if (!opened) return std::unexpected(opened.error());
        auto p = makePlan(*opened, req, &identity);
        if (!p) return std::unexpected(p.error());
        plan = std::move(*p);
        dataRef = plan.catalogRef;
    }

    const bool wantTimestamp = static_cast<bool>(req.timestamp.fetch);
    std::size_t reserve = req.initialReserveOverride;
    if (reserve == 0) {
        reserve = kDefaultReserveBase + req.extraReserveBytes;
        if (req.includeChain)
            for (const crypto::Bytes& c : identity.chainDer()) reserve += c.size();
        else
            reserve += identity.certificate().der.size();
        reserve += identity.certificate().der.size(); // ESS hash + issuer/serial headroom
        if (wantTimestamp) reserve += tokenReserve(req.timestamp.expectedTokenBytes);
    }
    // A timestamped attempt costs a TSA round trip: at most one rebuild, and only if the request allows it.
    const std::size_t maxAttempts = wantTimestamp ? (req.timestamp.retryWithLargerReservation ? 2 : 1) : kMaxAttempts;

    crypto::CmsSignOptions co;
    co.digest = req.digest;
    co.signature = req.signature;
    co.cadesBes = plan.pades;
    co.includeSigningTime = !plan.pades;
    co.includeChain = req.includeChain;
    co.signingTime = req.signingTime;
    co.cancel = req.cancel;

    SignedPdf out;
    out.inputSize = pdf.size();
    out.signatureRef = PdfRef{plan.sigNum, 0};
    out.fieldName = plan.fieldName;
    out.classicXref = plan.xrefNum == 0;

    for (std::size_t attempt = 1; attempt <= maxAttempts; ++attempt) {
        if (req.cancel.cancelled()) return fail(SignErrorCode::Cancelled, "cancelled");
        if (reserve > kMaxReserve) return fail(SignErrorCode::SignatureTooLarge, "signature exceeds the size limit");
        auto layout = layoutRevision(plan, req, baseSize, needPad, reserve, dataRef);
        if (!layout) return std::unexpected(layout.error());
        Layout& L = *layout;

        // The signed bytes: the original file, then the new revision around the /Contents hole.
        const ByteView prefix(pdf.data(), pdf.size());
        const ByteView head(reinterpret_cast<const std::uint8_t*>(L.rev.data()), L.contentsStart);
        const ByteView tail(reinterpret_cast<const std::uint8_t*>(L.rev.data()) + L.contentsEnd,
                            L.rev.size() - L.contentsEnd);
        const crypto::ContentSource content = crypto::ContentSource::fromSpans({prefix, head, tail});

        auto cms = crypto::signCms(provider, identity, content, /*detached=*/true, co);
        if (!cms) return failCrypto(cms.error());
        if (req.finalizeCms) {
            if (auto r = req.finalizeCms(*cms); !r) return failCrypto(r.error());
        }
        TimestampOutcome stamp;
        if (wantTimestamp) {
            auto st = applySignatureTimestamp(*cms, req.timestamp, provider, req.cancel);
            if (!st) return std::unexpected(st.error());
            stamp = std::move(*st);
        }
        auto der = crypto::encodeCms(*cms);
        if (!der) return failCrypto(der.error());

        if (der->size() > reserve) {
            if (!wantTimestamp) {
                reserve = der->size() + 128 + attempt * 256; // never shift bytes after the digest: rebuild the revision
                out.attempts = attempt + 1;
                continue;
            }
            if (attempt < maxAttempts) {
                reserve = der->size() + 512; // the token size is known now (a re-issued token varies a little)
                out.attempts = attempt + 1;
                continue;
            }
            SignError e{SignErrorCode::ContentsPlaceholderTooSmall,
                        "the signature with its time-stamp (" + std::to_string(der->size()) +
                            " bytes) does not fit the reserved /Contents space (" + std::to_string(reserve) + " bytes)",
                        std::nullopt};
            e.requiredBytes = der->size();
            return std::unexpected<SignError>(std::move(e));
        }
        fillContents(L, *der);
        out.cmsBytes = der->size();
        out.reservedBytes = reserve;
        out.attempts = attempt;
        out.timestamped = stamp.attached;
        out.timestampTokenBytes = stamp.tokenBytes;
        out.timestampSkippedReason = std::move(stamp.skippedReason);
        out.level = !plan.pades ? PadesLevel::NotPades : stamp.attached ? PadesLevel::BaselineT : PadesLevel::BaselineB;
        pdf.insert(pdf.end(), L.rev.begin(), L.rev.end());
        out.data = std::move(pdf);

        // Self-verification of the produced file.
        auto verified = verifyPdfSignatures(out.data, provider);
        if (!verified) return fail(SignErrorCode::SelfVerifyFailed, "signed file is unreadable: " + verified.error().message);
        const PdfSignatureInfo* mine = nullptr;
        for (const PdfSignatureInfo& s : *verified)
            if (s.signatureRef && *s.signatureRef == out.signatureRef) mine = &s;
        if (!mine || !mine->byteRangeValid || mine->validation.crypto != crypto::CryptoCheck::Valid ||
            mine->validation.integrity != crypto::IntegrityStatus::Intact || !mine->coversWholeFile)
            return fail(SignErrorCode::SelfVerifyFailed, "the new signature did not verify");
        if (plan.pades && !mine->padesBaselineB)
            return fail(SignErrorCode::SelfVerifyFailed, "the new signature is not a PAdES B-B container");
        if (out.timestamped) {
            if (!mine->hasTimestampToken || !mine->timestamp || !mine->timestamp->genTime)
                return fail(SignErrorCode::SelfVerifyFailed, "the embedded signature time-stamp is not readable");
            out.timestampGenTime = mine->timestamp->genTime;
            out.timestampTsaSubject = mine->timestamp->tsaSubject;
        }
        return out;
    }
    return fail(SignErrorCode::SignatureTooLarge, "the CMS did not fit the reserved /Contents space");
}

SignResult<SignedPdf> timestampPdf(Bytes pdf, const DocTimestampRequest& req, crypto::CryptoProvider& provider) {
    if (req.cancel.cancelled()) return fail(SignErrorCode::Cancelled, "cancelled");
    if (!req.fetch) return fail(SignErrorCode::InvalidArgument, "no time-stamp source supplied");
    if (auto ok = crypto::checkCreationAllowed(req.digest); !ok) return failCrypto(ok.error());
    if (req.placement.kind == SignaturePlacement::Kind::NewVisible)
        return fail(SignErrorCode::InvalidArgument, "a document time-stamp field is always invisible");
    if (pdf.size() > kMaxSignableFileSize - (std::uint64_t{16} << 20))
        return fail(SignErrorCode::LimitExceeded, "file too large to time-stamp (limit 2 GiB)");

    const std::uint64_t baseSize = pdf.size();
    const bool needPad = pdf.empty() || (pdf.back() != '\n' && pdf.back() != '\r');
    SignRequest shape; // only the placement matters for the plan
    shape.placement = req.placement;
    Plan plan;
    {
        auto opened = PdfFile::open(pdf);
        if (!opened) return std::unexpected(opened.error());
        auto p = makePlan(*opened, shape, nullptr, /*docTs=*/true);
        if (!p) return std::unexpected(p.error());
        plan = std::move(*p);
    }
    std::size_t reserve = req.initialReserveOverride != 0 ? req.initialReserveOverride : tokenReserve(req.expectedTokenBytes);
    const std::size_t maxAttempts = req.retryWithLargerReservation ? 2 : 1;

    SignedPdf out;
    out.inputSize = pdf.size();
    out.signatureRef = PdfRef{plan.sigNum, 0};
    out.fieldName = plan.fieldName;
    out.classicXref = plan.xrefNum == 0;
    out.level = PadesLevel::NotPades;

    for (std::size_t attempt = 1; attempt <= maxAttempts; ++attempt) {
        if (req.cancel.cancelled()) return fail(SignErrorCode::Cancelled, "cancelled");
        if (reserve > kMaxReserve) return fail(SignErrorCode::SignatureTooLarge, "time-stamp token exceeds the size limit");
        auto layout = layoutRevision(plan, shape, baseSize, needPad, reserve, plan.catalogRef);
        if (!layout) return std::unexpected(layout.error());
        Layout& L = *layout;

        const ByteView prefix(pdf.data(), pdf.size());
        const ByteView head(reinterpret_cast<const std::uint8_t*>(L.rev.data()), L.contentsStart);
        const ByteView tail(reinterpret_cast<const std::uint8_t*>(L.rev.data()) + L.contentsEnd,
                            L.rev.size() - L.contentsEnd);
        const crypto::ContentSource content = crypto::ContentSource::fromSpans({prefix, head, tail});
        auto digest = content.digest(provider, req.digest, req.cancel);
        if (!digest) return failCrypto(digest.error());

        auto fetched = req.fetch(req.digest, *digest, req.cancel);
        if (!fetched) {
            if (fetched.error().code == crypto::CryptoErrorCode::Cancelled) return failCrypto(fetched.error());
            return std::unexpected<SignError>(
                SignError{SignErrorCode::TimestampFailed,
                          "the time-stamp request failed: " + std::string(crypto::toString(fetched.error().code)) + ": " +
                              fetched.error().message,
                          fetched.error().code});
        }
        if (!fetched->ok()) {
            std::string why = "the time-stamp token was rejected (" + std::string(crypto::toString(fetched->failure)) + ")";
            const std::string notes = joinNotes(fetched->validation.notes);
            if (!notes.empty()) why += ": " + notes;
            return fail(SignErrorCode::TimestampFailed, std::move(why));
        }
        const Bytes& token = fetched->tokenDer;
        if (token.size() > reserve) {
            if (attempt < maxAttempts) {
                reserve = token.size() + 512;
                out.attempts = attempt + 1;
                continue;
            }
            SignError e{SignErrorCode::ContentsPlaceholderTooSmall,
                        "the time-stamp token (" + std::to_string(token.size()) +
                            " bytes) does not fit the reserved /Contents space (" + std::to_string(reserve) + " bytes)",
                        std::nullopt};
            e.requiredBytes = token.size();
            return std::unexpected<SignError>(std::move(e));
        }
        fillContents(L, token);
        out.cmsBytes = out.timestampTokenBytes = token.size();
        out.reservedBytes = reserve;
        out.attempts = attempt;
        out.timestamped = true;
        pdf.insert(pdf.end(), L.rev.begin(), L.rev.end());
        out.data = std::move(pdf);

        auto verified = verifyPdfSignatures(out.data, provider);
        if (!verified) return fail(SignErrorCode::SelfVerifyFailed, "time-stamped file is unreadable: " + verified.error().message);
        const PdfSignatureInfo* mine = nullptr;
        for (const PdfSignatureInfo& s : *verified)
            if (s.signatureRef && *s.signatureRef == out.signatureRef) mine = &s;
        if (!mine || mine->kind != SignatureKind::DocumentTimestamp || !mine->byteRangeValid ||
            mine->validation.crypto != crypto::CryptoCheck::Valid ||
            mine->validation.integrity != crypto::IntegrityStatus::Intact || !mine->coversWholeFile || !mine->timestamp ||
            !mine->timestamp->genTime)
            return fail(SignErrorCode::SelfVerifyFailed, "the new document time-stamp did not verify");
        out.timestampGenTime = mine->timestamp->genTime;
        out.timestampTsaSubject = mine->timestamp->tsaSubject;
        return out;
    }
    return fail(SignErrorCode::SignatureTooLarge, "the time-stamp token did not fit the reserved /Contents space");
}

namespace {

SignResult<Bytes> readWhole(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return fail(SignErrorCode::Io, "cannot stat the file");
    if (size > kMaxSignableFileSize) return fail(SignErrorCode::LimitExceeded, "file too large to sign (limit 2 GiB)");
    std::ifstream in(path, std::ios::binary);
    if (!in) return fail(SignErrorCode::Io, "cannot open the file");
    Bytes data(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (static_cast<std::size_t>(in.gcount()) != data.size()) return fail(SignErrorCode::Io, "short read");
    return data;
}

} // namespace

namespace {

// Reads `input`, lets `produce` build the new file, writes it to a temporary
// file next to `output`, reads it back, checks that the input bytes are an
// exact prefix and that the new signature verifies, then renames atomically.
SignResult<SignedPdf> transactionalFile(const std::filesystem::path& input, const std::filesystem::path& output,
                                        crypto::CryptoProvider& provider,
                                        const std::function<SignResult<SignedPdf>(Bytes)>& produce) {
    auto source = readWhole(input);
    if (!source) return std::unexpected(source.error());
    const std::size_t inputSize = source->size();
    auto before = provider.digest(crypto::DigestAlgorithm::Sha256, *source);
    if (!before) return failCrypto(before.error());

    auto signedPdf = produce(std::move(*source));
    if (!signedPdf) return std::unexpected(signedPdf.error());

    auto writer = core::io::AtomicFileWriter::begin(output);
    if (!writer) return fail(SignErrorCode::Io, "cannot create the output file: " + writer.error().message);
    auto abort = [&](SignError e) -> std::unexpected<SignError> {
        (*writer)->abort();
        return std::unexpected<SignError>(std::move(e));
    };
    if (auto w = (*writer)->write(signedPdf->data.data(), signedPdf->data.size()); !w)
        return abort(SignError{SignErrorCode::Io, "cannot write the output file: " + w.error().message, std::nullopt});

    // Read the temporary file back and verify it before it replaces anything.
    const std::filesystem::path temp = (*writer)->tempPath();
    const std::size_t signedSize = signedPdf->data.size();
    signedPdf->data = Bytes{}; // release before reading back
    auto back = readWhole(temp);
    if (!back || back->size() != signedSize) return abort(SignError{SignErrorCode::Io, "read-back of the temporary file failed", std::nullopt});
    auto after = provider.digest(crypto::DigestAlgorithm::Sha256, ByteView(back->data(), inputSize));
    if (!after || *after != *before)
        return abort(SignError{SignErrorCode::SelfVerifyFailed, "the original bytes changed in the output", std::nullopt});
    auto verified = verifyPdfSignatures(*back, provider);
    bool ok = false;
    if (verified)
        for (const PdfSignatureInfo& s : *verified)
            if (s.signatureRef && *s.signatureRef == signedPdf->signatureRef &&
                s.validation.crypto == crypto::CryptoCheck::Valid && s.validation.integrity == crypto::IntegrityStatus::Intact &&
                s.coversWholeFile)
                ok = true;
    if (!ok) return abort(SignError{SignErrorCode::SelfVerifyFailed, "the written file did not verify", std::nullopt});
    back->clear();

    if (auto c = (*writer)->commit(); !c)
        return abort(SignError{SignErrorCode::Io, "cannot commit the output file: " + c.error().message, std::nullopt});
    signedPdf->inputSize = inputSize;
    return signedPdf;
}


} // namespace

SignResult<SignedPdf> signPdfFile(const std::filesystem::path& input, const std::filesystem::path& output,
                                  const SignRequest& req, crypto::CryptoProvider& provider,
                                  crypto::SigningIdentity& identity) {
    return transactionalFile(input, output, provider,
                             [&](Bytes data) { return signPdf(std::move(data), req, provider, identity); });
}

SignResult<SignedPdf> timestampPdfFile(const std::filesystem::path& input, const std::filesystem::path& output,
                                       const DocTimestampRequest& req, crypto::CryptoProvider& provider) {
    return transactionalFile(input, output, provider,
                             [&](Bytes data) { return timestampPdf(std::move(data), req, provider); });
}

} // namespace rivet::pdf::sign
