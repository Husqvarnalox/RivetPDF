// SPDX-License-Identifier: MPL-2.0
#include "pdf/sign/PdfSignatureVerifier.hpp"

#include <algorithm>
#include <cstring>
#include <set>
#include <sstream>

namespace rivet::pdf::sign {

namespace {

using crypto::CryptoCheck;
using crypto::IntegrityStatus;
using crypto::ModificationStatus;

constexpr std::size_t kMaxSignatures = 4096;
constexpr std::size_t kMaxNotes = 12;

// ---- small object helpers ------------------------------------------------------------

bool same(const PdfObject& a, const PdfObject& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
    case PdfObject::Kind::Null: return true;
    case PdfObject::Kind::Bool: return a.boolean == b.boolean;
    case PdfObject::Kind::Int: return a.integer == b.integer;
    case PdfObject::Kind::Real:
    case PdfObject::Kind::Name:
    case PdfObject::Kind::String: return a.text == b.text;
    case PdfObject::Kind::Ref: return a.ref == b.ref;
    case PdfObject::Kind::Array: {
        if (!a.array || !b.array) return a.array == b.array;
        if (a.array->size() != b.array->size()) return false;
        for (std::size_t i = 0; i < a.array->size(); ++i)
            if (!same((*a.array)[i], (*b.array)[i])) return false;
        return true;
    }
    case PdfObject::Kind::Dict: {
        if (!a.dict || !b.dict) return a.dict == b.dict;
        if (a.dict->entries.size() != b.dict->entries.size()) return false;
        for (const auto& [k, v] : a.dict->entries) {
            const PdfObject* o = b.dict->find(k);
            if (!o || !same(v, *o)) return false;
        }
        return true;
    }
    }
    return false;
}

std::vector<std::string> diffKeys(const PdfDict& a, const PdfDict& b) {
    std::vector<std::string> out;
    for (const auto& [k, v] : a.entries) {
        const PdfObject* o = b.find(k);
        if (!o || !same(v, *o)) out.push_back(k);
    }
    for (const auto& [k, v] : b.entries)
        if (!a.find(k)) out.push_back(k);
    return out;
}

const PdfDict kEmptyDict;

const PdfDict& dictOrEmpty(const PdfObject& o) { return o.isDict() ? *o.dict : kEmptyDict; }

std::string nameOf(const PdfDict& d, std::string_view key) {
    const PdfObject* o = d.find(key);
    return o && o->isName() ? o->text : std::string();
}

std::string textOf(const PdfDict& d, std::string_view key) {
    const PdfObject* o = d.find(key);
    return o && o->isString() ? textStringToUtf8(o->text) : std::string();
}

// ---- DocMDP / FieldMDP -------------------------------------------------------------------

std::optional<DocMdpLevel> levelFromParams(const PdfDict& params) {
    const auto p = dictInt(params, "P");
    if (!p) return DocMdpLevel::FormFill; // ISO 32000-1: the default is 2
    if (*p == 1) return DocMdpLevel::NoChanges;
    if (*p == 2) return DocMdpLevel::FormFill;
    if (*p == 3) return DocMdpLevel::Annotate;
    return std::nullopt;
}

std::optional<FieldLock> lockFromDict(PdfFile& f, const PdfDict& d, std::size_t view) {
    const PdfObject* action = d.find("Action");
    if (!action || !action->isName()) return std::nullopt;
    FieldLock lock;
    if (action->text == "All") lock.action = FieldLockAction::All;
    else if (action->text == "Include") lock.action = FieldLockAction::Include;
    else if (action->text == "Exclude") lock.action = FieldLockAction::Exclude;
    else return std::nullopt;
    if (const PdfObject* fields = d.find("Fields")) {
        const PdfObject arr = f.resolve(*fields, view);
        if (arr.isArray())
            for (const PdfObject& e : *arr.array)
                if (e.isString()) lock.fields.push_back(textStringToUtf8(e.text));
    }
    return lock;
}

struct References {
    std::optional<DocMdpLevel> docMdp;
    bool docMdpPresent = false;
    std::optional<FieldLock> fieldMdp;
};

References readReferences(PdfFile& f, const PdfDict& sig, std::size_t view) {
    References out;
    const PdfObject* ref = sig.find("Reference");
    if (!ref) return out;
    const PdfObject arr = f.resolve(*ref, view);
    if (!arr.isArray()) return out;
    std::size_t n = 0;
    for (const PdfObject& e : *arr.array) {
        if (++n > 16) break;
        const PdfObject d = f.resolve(e, view);
        if (!d.isDict()) continue;
        const std::string method = nameOf(*d.dict, "TransformMethod");
        const PdfObject* tp = d.dict->find("TransformParams");
        const PdfObject params = tp ? f.resolve(*tp, view) : PdfObject::null();
        if (method == "DocMDP") {
            out.docMdpPresent = true;
            out.docMdp = params.isDict() ? levelFromParams(*params.dict) : DocMdpLevel::FormFill;
        } else if (method == "FieldMDP" && params.isDict()) {
            out.fieldMdp = lockFromDict(f, *params.dict, view);
        }
    }
    return out;
}

// The document's certification level as of `view`: /Perms /DocMDP -> signature -> /Reference.
std::optional<DocMdpLevel> docMdpOfView(PdfFile& f, std::size_t view, bool& present) {
    present = false;
    auto cat = f.catalog(view);
    if (!cat) return std::nullopt;
    const PdfObject* perms = cat->second.find("Perms");
    if (!perms) return std::nullopt;
    const PdfObject p = f.resolve(*perms, view);
    if (!p.isDict()) return std::nullopt;
    const PdfObject* dm = p.dict->find("DocMDP");
    if (!dm) return std::nullopt;
    const PdfObject sig = f.resolve(*dm, view);
    if (!sig.isDict()) return std::nullopt;
    const References r = readReferences(f, *sig.dict, view);
    if (!r.docMdpPresent) return std::nullopt;
    present = true;
    return r.docMdp;
}

// ---- modification analysis -----------------------------------------------------------------

enum ChangeClass : unsigned { kDss = 1, kSign = 2, kForm = 4, kAnnot = 8 };

class ModificationAnalyzer {
public:
    ModificationAnalyzer(PdfFile& file, std::size_t view, FieldLock lock) : f_(file), k_(view), lock_(std::move(lock)) {}

    struct Outcome {
        ModificationStatus status = ModificationStatus::None;
        std::vector<std::string> notes;
    };

    Outcome run() {
        Outcome out;
        if (!buildRoles()) {
            out.status = ModificationStatus::Unknown;
            out.notes.push_back("document structure of the signed revision could not be analysed");
            return out;
        }
        std::set<std::uint32_t> changed;
        for (std::size_t i = 0; i < k_; ++i) {
            const XrefSection& s = f_.section(i);
            for (const auto& [num, entry] : s.entries) {
                (void)entry;
                if (s.isStream && num == s.streamObjNum) continue;
                changed.insert(num);
            }
        }
        for (const std::uint32_t n : changed) {
            examine(n);
        }

        bool dm = false;
        const auto level = docMdpOfView(f_, k_, dm);
        const bool certified = dm;
        const unsigned lv = level ? static_cast<unsigned>(*level) : 0U;
        // P=1 forbids even signing; with no DocMDP, signing is the one permitted class.
        const bool allowSign = !certified || lv >= 2;
        const bool allowForm = certified && lv >= 2;
        const bool allowAnnot = certified && lv >= 3;
        if ((classes_ & kSign) && !allowSign) disallow("signature added to a document certified for no changes");
        if ((classes_ & kForm) && !allowForm) disallow("form fields changed after signing");
        if ((classes_ & kAnnot) && !allowAnnot) disallow("annotations changed after signing");

        out.notes = std::move(notes_);
        if (disallowed_) out.status = ModificationStatus::Disallowed;
        else if (unknown_) out.status = ModificationStatus::Unknown;
        else if (classes_ != 0) out.status = ModificationStatus::Permitted;
        else out.status = ModificationStatus::None;
        return out;
    }

private:
    struct Snapshot {
        std::optional<PdfObject> value;
        bool isStream = false;
        std::uint64_t sBegin = 0, sLen = 0;
        bool ok = true;
    };

    void note(const std::string& s) {
        if (notes_.size() < kMaxNotes) notes_.push_back(s);
    }
    void disallow(const std::string& s) {
        disallowed_ = true;
        note(s);
    }
    void unknown(const std::string& s) {
        unknown_ = true;
        note(s);
    }

    Snapshot snap(std::uint32_t n, std::size_t view) {
        Snapshot s;
        auto v = f_.loadObject(n, view);
        if (!v) {
            s.ok = false;
            return s;
        }
        s.value = std::move(*v);
        const auto e = f_.lookup(n, view);
        if (e && e->type == XrefEntry::Type::InUse && s.value) {
            if (auto ind = f_.loadIndirect(n, view); ind && ind->isStream) {
                s.isStream = true;
                s.sBegin = ind->streamBegin;
                s.sLen = ind->streamLength;
            }
        }
        return s;
    }

    PdfDict dictAt(const PdfObject& o, std::size_t view) { return dictOrEmpty(f_.resolve(o, view)); }

    bool isSigField(const PdfDict& d) { return nameOf(d, "FT") == "Sig"; }

    bool buildRoles() {
        auto cat = f_.catalog(k_);
        if (!cat) return false;
        catalogNum_ = cat->first.num;
        const PdfDict& cd = cat->second;
        if (const PdfObject* af = cd.find("AcroForm"); af && af->isRef()) {
            acroformNum_ = af->ref.num;
            const PdfObject a = f_.resolve(*af, k_);
            if (a.isDict())
                if (const PdfObject* fl = a.dict->find("Fields"); fl && fl->isRef()) fieldsArrayNum_ = fl->ref.num;
        } else if (af && af->isDict()) {
            if (const PdfObject* fl = af->dict->find("Fields"); fl && fl->isRef()) fieldsArrayNum_ = fl->ref.num;
        }
        auto fields = f_.fields(k_);
        if (!fields) return false;
        for (const FieldInfo& fi : *fields) {
            const bool sig = fi.fieldType == "Sig";
            std::vector<std::uint32_t> objs;
            if (fi.ref) objs.push_back(fi.ref->num);
            for (const PdfRef& w : fi.widgets) objs.push_back(w.num);
            for (const std::uint32_t o : objs) {
                fieldObjs_.insert(o);
                fieldName_[o] = fi.fullName;
                if (sig) sigFieldObjs_.insert(o);
            }
            if (sig) {
                const PdfObject* v = fi.dict.find("V");
                if (v && !v->isNull()) {
                    for (const std::uint32_t o : objs) signedFieldObjs_.insert(o);
                    if (v->isRef()) sigDictObjs_.insert(v->ref.num);
                }
            }
            // appearance streams of widgets
            auto addAp = [&](const PdfDict& wd) {
                const PdfObject* ap = wd.find("AP");
                if (!ap) return;
                const PdfObject apd = f_.resolve(*ap, k_);
                if (!apd.isDict()) return;
                for (const auto& [key, val] : apd.dict->entries) {
                    (void)key;
                    if (val.isRef()) {
                        apStreams_.insert(val.ref.num);
                        const PdfObject sub = f_.resolve(val, k_);
                        if (sub.isDict() && !sub.dict->isType("XObject"))
                            for (const auto& [k2, v2] : sub.dict->entries) {
                                (void)k2;
                                if (v2.isRef()) apStreams_.insert(v2.ref.num);
                            }
                    } else if (val.isDict()) {
                        for (const auto& [k2, v2] : val.dict->entries) {
                            (void)k2;
                            if (v2.isRef()) apStreams_.insert(v2.ref.num);
                        }
                    }
                }
            };
            addAp(fi.dict);
            for (const PdfRef& w : fi.widgets) addAp(dictAt(PdfObject::makeRef(w), k_));
        }
        if (const PdfObject* perms = cd.find("Perms")) {
            const PdfObject p = f_.resolve(*perms, k_);
            if (p.isDict())
                for (const auto& [key, val] : p.dict->entries) {
                    (void)key;
                    if (val.isRef()) sigDictObjs_.insert(val.ref.num);
                }
        }
        // Page tree.
        const PdfObject* pagesObj = cd.find("Pages");
        if (!pagesObj || !pagesObj->isRef()) return false;
        std::vector<std::pair<PdfRef, int>> stack{{pagesObj->ref, 0}};
        std::set<std::uint32_t> visited;
        while (!stack.empty()) {
            const auto [ref, depth] = stack.back();
            stack.pop_back();
            if (depth > 64 || !visited.insert(ref.num).second) continue;
            if (visited.size() > kMaxFieldNodes * 5) return false;
            auto obj = f_.loadObject(ref.num, k_);
            if (!obj || !*obj || !(*obj)->isDict()) continue;
            const PdfDict& d = *(*obj)->dict;
            const PdfObject kids = d.find("Kids") ? f_.resolve(*d.find("Kids"), k_) : PdfObject::null();
            if (kids.isArray() && !d.isType("Page")) {
                pageTreeNodes_.insert(ref.num);
                for (const PdfObject& kid : *kids.array)
                    if (kid.isRef()) stack.push_back({kid.ref, depth + 1});
                continue;
            }
            pageNums_.insert(ref.num);
            if (const PdfObject* an = d.find("Annots")) {
                if (an->isRef()) annotsArrays_.insert(an->ref.num);
                const PdfObject arr = f_.resolve(*an, k_);
                if (arr.isArray())
                    for (const PdfObject& e : *arr.array)
                        if (e.isRef()) annotObjs_.insert(e.ref.num);
            }
        }
        return true;
    }

    // Is `name` covered by the signature's field lock?
    bool locked(const std::string& name) const {
        switch (lock_.action) {
        case FieldLockAction::None: return false;
        case FieldLockAction::All: return true;
        case FieldLockAction::Include: return std::find(lock_.fields.begin(), lock_.fields.end(), name) != lock_.fields.end();
        case FieldLockAction::Exclude: return std::find(lock_.fields.begin(), lock_.fields.end(), name) == lock_.fields.end();
        }
        return false;
    }

    void examine(std::uint32_t n) {
        Snapshot before = snap(n, k_);
        Snapshot after = snap(n, 0);
        if (!before.ok || !after.ok) {
            unknown("object " + std::to_string(n) + " could not be compared");
            return;
        }
        if (!before.value) {
            if (after.value && after.value->isDict() && after.value->dict->isType("XRef")) return;
            return; // a new object: only matters through the objects that reference it
        }
        if (!after.value) {
            disallow("object " + std::to_string(n) + " was deleted");
            return;
        }
        if (before.isStream || after.isStream) {
            const bool sameData =
                before.isStream && after.isStream && before.sLen == after.sLen &&
                (before.sLen == 0 || std::memcmp(f_.data().data() + before.sBegin, f_.data().data() + after.sBegin,
                                                 static_cast<std::size_t>(before.sLen)) == 0);
            if (sameData && same(*before.value, *after.value)) return;
            if (apStreams_.count(n)) {
                classes_ |= kForm;
                if (locked(fieldNameOfAp(n))) disallow("appearance of a locked field was changed");
            } else {
                disallow("stream object " + std::to_string(n) + " was modified");
            }
            return;
        }
        if (same(*before.value, *after.value)) return;
        const PdfObject& b = *before.value;
        const PdfObject& a = *after.value;

        if (n == catalogNum_) return diffCatalog(dictOrEmpty(b), dictOrEmpty(a));
        if (n == acroformNum_) return diffAcroForm(dictOrEmpty(b), dictOrEmpty(a));
        if (n == fieldsArrayNum_) return diffFieldsArray(b, a);
        if (sigDictObjs_.count(n)) return disallow("signature dictionary " + std::to_string(n) + " was modified");
        if (signedFieldObjs_.count(n)) return disallow("a signed signature field was modified");
        if (fieldObjs_.count(n)) return diffField(n, dictOrEmpty(b), dictOrEmpty(a));
        if (pageNums_.count(n)) return diffPage(n, dictOrEmpty(b), dictOrEmpty(a));
        if (annotsArrays_.count(n)) return diffAnnots(b, a);
        if (annotObjs_.count(n)) {
            classes_ |= kAnnot;
            return;
        }
        if (pageTreeNodes_.count(n)) return disallow("page tree node " + std::to_string(n) + " was modified");
        unknown("object " + std::to_string(n) + " was modified (not classified)");
    }

    std::string fieldNameOfAp(std::uint32_t apObj) {
        (void)apObj;
        // Appearance streams are shared rarely; attribute to the lock check conservatively by scanning fields.
        for (const std::uint32_t fo : fieldObjs_) {
            const auto it = fieldName_.find(fo);
            if (it == fieldName_.end()) continue;
            auto d = f_.loadObject(fo, k_);
            if (!d || !*d || !(*d)->isDict()) continue;
            const PdfObject* ap = (*d)->dict->find("AP");
            if (!ap) continue;
            const PdfObject apd = f_.resolve(*ap, k_);
            if (!apd.isDict()) continue;
            for (const auto& [key, val] : apd.dict->entries) {
                (void)key;
                if (val.isRef() && val.ref.num == apObj) return it->second;
            }
        }
        return {};
    }

    void diffCatalog(const PdfDict& b, const PdfDict& a) {
        static const std::set<std::string> kDisallowed = {"Pages",        "Names",    "Outlines",   "OpenAction",
                                                          "AA",           "Metadata", "PageLabels", "StructTreeRoot",
                                                          "OCProperties", "Dests",    "Threads",    "Perms",
                                                          "Legal",        "Lang"};
        for (const std::string& key : diffKeys(b, a)) {
            if (key == "DSS" || key == "Extensions" || key == "Version") {
                classes_ |= kDss;
            } else if (key == "AcroForm") {
                const PdfObject* bo = b.find(key);
                const PdfObject* ao = a.find(key);
                if ((!bo || bo->isDict()) && ao && ao->isDict()) diffAcroForm(bo ? *bo->dict : kEmptyDict, *ao->dict);
                else unknown("the /AcroForm entry of the catalog was replaced");
            } else if (kDisallowed.count(key)) {
                disallow("catalog entry /" + key + " was changed");
            } else {
                unknown("catalog entry /" + key + " was changed");
            }
        }
    }

    void diffAcroForm(const PdfDict& b, const PdfDict& a) {
        for (const std::string& key : diffKeys(b, a)) {
            if (key == "Fields") {
                const PdfObject* bo = b.find(key);
                const PdfObject* ao = a.find(key);
                if (bo && ao && bo->isArray() && ao->isArray()) diffFieldsArray(*bo, *ao);
                else if (!bo && ao && ao->isArray()) diffFieldsArray(PdfObject::makeArray({}), *ao);
                else unknown("the /Fields entry of the AcroForm was replaced");
            } else if (key == "SigFlags") {
                classes_ |= kSign;
            } else if (key == "DA" || key == "DR" || key == "Q" || key == "NeedAppearances") {
                classes_ |= kForm;
            } else if (key == "XFA") {
                disallow("the XFA form data was changed");
            } else {
                unknown("AcroForm entry /" + key + " was changed");
            }
        }
    }

    static std::set<std::uint32_t> refNums(const PdfObject& arr) {
        std::set<std::uint32_t> out;
        if (arr.isArray())
            for (const PdfObject& e : *arr.array)
                if (e.isRef()) out.insert(e.ref.num);
        return out;
    }

    static bool hasDirectElements(const PdfObject& arr) {
        if (arr.isArray())
            for (const PdfObject& e : *arr.array)
                if (!e.isRef()) return true;
        return false;
    }

    void diffFieldsArray(const PdfObject& b, const PdfObject& a) {
        if (hasDirectElements(b) || hasDirectElements(a)) return unknown("form field array holds direct objects");
        const auto before = refNums(b);
        const auto after = refNums(a);
        for (const std::uint32_t n : before)
            if (!after.count(n)) return disallow("a form field was removed");
        for (const std::uint32_t n : after) {
            if (before.count(n)) continue;
            const PdfDict d = dictAt(PdfObject::makeRef({n, 0}), 0);
            if (isSigField(d)) classes_ |= kSign;
            else disallow("a non-signature form field was added");
        }
    }

    void diffField(std::uint32_t n, const PdfDict& b, const PdfDict& a) {
        const bool sig = sigFieldObjs_.count(n) != 0;
        const auto it = fieldName_.find(n);
        const std::string name = it == fieldName_.end() ? std::string() : it->second;
        for (const std::string& key : diffKeys(b, a)) {
            if (sig) {
                // An empty signature field being signed: its /V and appearance change.
                if (key == "V" || key == "AP" || key == "AS") classes_ |= kSign;
                else disallow("signature field entry /" + key + " was changed");
            } else if (key == "V" || key == "AP" || key == "AS") {
                classes_ |= kForm;
                if (locked(name)) disallow("locked field \"" + name + "\" was changed");
            } else {
                disallow("form field entry /" + key + " was changed");
            }
        }
    }

    void diffPage(std::uint32_t n, const PdfDict& b, const PdfDict& a) {
        (void)n;
        for (const std::string& key : diffKeys(b, a)) {
            if (key == "Annots") {
                const PdfObject* bo = b.find(key);
                const PdfObject* ao = a.find(key);
                const PdfObject empty = PdfObject::makeArray({});
                const PdfObject bArr = bo ? (bo->isRef() ? f_.resolve(*bo, k_) : *bo) : empty;
                const PdfObject aArr = ao ? (ao->isRef() ? f_.resolve(*ao, 0) : *ao) : empty;
                diffAnnots(bArr, aArr);
            } else {
                disallow("page entry /" + key + " was changed");
            }
        }
    }

    void diffAnnots(const PdfObject& b, const PdfObject& a) {
        if (!b.isArray() || !a.isArray() || hasDirectElements(b) || hasDirectElements(a))
            return unknown("annotation array could not be compared");
        const auto before = refNums(b);
        const auto after = refNums(a);
        for (const std::uint32_t n : before) {
            if (after.count(n)) continue;
            if (fieldObjs_.count(n)) return disallow("a form field widget was removed");
            classes_ |= kAnnot;
        }
        for (const std::uint32_t n : after) {
            if (before.count(n)) continue;
            const PdfDict d = dictAt(PdfObject::makeRef({n, 0}), 0);
            if (nameOf(d, "Subtype") == "Widget") {
                if (isSigField(d)) classes_ |= kSign;
                else if (d.find("Parent")) classes_ |= kSign; // widget of a new signature field below a parent
                else disallow("a non-signature widget was added");
            } else {
                classes_ |= kAnnot;
            }
        }
    }

    PdfFile& f_;
    std::size_t k_;
    FieldLock lock_;
    std::uint32_t catalogNum_ = 0, acroformNum_ = 0, fieldsArrayNum_ = 0;
    std::set<std::uint32_t> fieldObjs_, sigFieldObjs_, signedFieldObjs_, sigDictObjs_, apStreams_;
    std::set<std::uint32_t> pageNums_, pageTreeNodes_, annotsArrays_, annotObjs_;
    std::map<std::uint32_t, std::string> fieldName_;
    unsigned classes_ = 0;
    bool disallowed_ = false;
    bool unknown_ = false;
    std::vector<std::string> notes_;
};

// ---- CMS framing ------------------------------------------------------------------------------

// Total length of a DER/BER SEQUENCE at the start of `data`, or nullopt (indefinite or malformed).
std::optional<std::size_t> derTotalLength(const Bytes& data) {
    if (data.size() < 2 || data[0] != 0x30) return std::nullopt;
    const std::uint8_t first = data[1];
    std::uint64_t len = 0;
    std::size_t header = 2;
    if (first < 0x80) {
        len = first;
    } else if (first == 0x80) {
        return std::nullopt;
    } else {
        const std::size_t n = first & 0x7F;
        if (n > 4 || data.size() < 2 + n) return std::nullopt;
        for (std::size_t i = 0; i < n; ++i) len = (len << 8) | data[2 + i];
        header += n;
    }
    const std::uint64_t total = len + header;
    if (total > data.size()) return std::nullopt;
    return static_cast<std::size_t>(total);
}

struct Found {
    std::string fieldName;
    PdfRef ref;
    std::optional<FieldLock> fieldLock;
    bool duplicate = false;
};

} // namespace

SignResult<std::vector<PdfSignatureInfo>> verifyPdfSignatures(ByteView pdf, crypto::CryptoProvider& provider,
                                                              const VerifyOptions& options) {
    auto opened = PdfFile::open(pdf);
    if (!opened) return std::unexpected(opened.error());
    PdfFile& f = *opened;

    std::vector<Found> found;
    std::set<std::uint32_t> seen;
    auto addRef = [&](const PdfRef& r, std::string name, std::optional<FieldLock> lock) {
        Found fd;
        fd.fieldName = std::move(name);
        fd.ref = r;
        fd.fieldLock = std::move(lock);
        fd.duplicate = !seen.insert(r.num).second;
        found.push_back(std::move(fd));
    };

    {
        auto fields = f.fields(0);
        if (!fields) return std::unexpected(fields.error());
        for (const FieldInfo& fi : *fields) {
            if (fi.fieldType != "Sig") continue;
            const PdfObject* v = fi.dict.find("V");
            if (!v || v->isNull() || !v->isRef()) continue;
            std::optional<FieldLock> lock;
            if (const PdfObject* l = fi.dict.find("Lock")) {
                const PdfObject ld = f.resolve(*l, 0);
                if (ld.isDict()) lock = lockFromDict(f, *ld.dict, 0);
            }
            addRef(v->ref, fi.fullName, std::move(lock));
            if (found.size() > kMaxSignatures) return fail(SignErrorCode::LimitExceeded, "too many signatures");
        }
    }
    {
        auto cat = f.catalog(0);
        if (!cat) return std::unexpected(cat.error());
        if (const PdfObject* perms = cat->second.find("Perms")) {
            const PdfObject p = f.resolve(*perms, 0);
            if (p.isDict())
                if (const PdfObject* dm = p.dict->find("DocMDP"); dm && dm->isRef() && !seen.count(dm->ref.num))
                    addRef(dm->ref, "", std::nullopt);
        }
    }

    bool permsDm = false;
    const auto catalogLevel = docMdpOfView(f, 0, permsDm);
    (void)catalogLevel;
    std::optional<std::uint32_t> docMdpSigNum;
    if (auto cat = f.catalog(0)) {
        if (const PdfObject* perms = cat->second.find("Perms")) {
            const PdfObject p = f.resolve(*perms, 0);
            if (p.isDict())
                if (const PdfObject* dm = p.dict->find("DocMDP"); dm && dm->isRef()) docMdpSigNum = dm->ref.num;
        }
    }

    std::vector<PdfSignatureInfo> results;
    results.reserve(found.size());
    const std::uint64_t fileSize = pdf.size();

    for (std::size_t idx = 0; idx < found.size(); ++idx) {
        const Found& fd = found[idx];
        PdfSignatureInfo info;
        info.index = idx;
        info.fieldName = fd.fieldName;
        info.signatureRef = fd.ref;
        crypto::ValidationResult& v = info.validation;

        auto finishBad = [&](const std::string& why) {
            v.crypto = CryptoCheck::Invalid;
            v.integrity = IntegrityStatus::Unknown;
            v.notes.push_back(why);
            v.state = crypto::foldState(v);
            results.push_back(info);
        };

        if (options.cancel.cancelled()) return fail(SignErrorCode::Cancelled, "verification cancelled");
        if (fd.duplicate) {
            finishBad("signature dictionary referenced by more than one field");
            continue;
        }
        const auto entry = f.lookup(fd.ref.num, 0);
        if (!entry || entry->type != XrefEntry::Type::InUse) {
            finishBad("signature dictionary is missing or stored in an object stream");
            continue;
        }
        auto ind = parseIndirectObjectAt(pdf, entry->a);
        if (!ind || !ind->value.isDict() || ind->isStream) {
            finishBad("signature dictionary is not a plain dictionary object");
            continue;
        }
        const PdfDict& sd = *ind->value.dict;
        if (sd.duplicateKeys) {
            finishBad("signature dictionary has duplicate keys");
            continue;
        }
        info.type = nameOf(sd, "Type");
        info.subFilter = nameOf(sd, "SubFilter");
        info.reason = textOf(sd, "Reason");
        info.location = textOf(sd, "Location");
        info.signerName = textOf(sd, "Name");
        info.contactInfo = textOf(sd, "ContactInfo");
        if (const PdfObject* m = sd.find("M"); m && m->isString()) info.signingTimeText = m->text;

        const References refs = readReferences(f, sd, 0);
        info.fieldMdp = refs.fieldMdp;
        if (docMdpSigNum && *docMdpSigNum == fd.ref.num && refs.docMdpPresent && refs.docMdp)
            info.certification = *refs.docMdp;

        // ---- ByteRange: hostile until proven otherwise
        const PdfObject* br = sd.find("ByteRange");
        if (!br || !br->isArray()) {
            finishBad("missing /ByteRange");
            continue;
        }
        bool intsOnly = true;
        for (const PdfObject& e : *br->array) {
            if (!e.isInt()) {
                intsOnly = false;
                break;
            }
            info.byteRange.push_back(e.integer);
        }
        if (!intsOnly || info.byteRange.size() != 4) {
            finishBad("/ByteRange must be four integers");
            continue;
        }
        const std::int64_t r0 = info.byteRange[0], l1 = info.byteRange[1], o2 = info.byteRange[2], l2 = info.byteRange[3];
        const auto fs = static_cast<std::int64_t>(fileSize);
        if (r0 != 0 || l1 < 0 || o2 < 0 || l2 <= 0 || l1 > fs || o2 > fs || l2 > fs) {
            finishBad("/ByteRange is negative, out of bounds or does not start at 0");
            continue;
        }
        if (o2 < l1) {
            finishBad("/ByteRange ranges overlap");
            continue;
        }
        if (o2 + l2 > fs) {
            finishBad("/ByteRange extends past the end of the file");
            continue;
        }
        const PdfObject* contents = sd.find("Contents");
        if (!contents || !contents->isString() || !contents->hexString) {
            finishBad("missing or non-hex /Contents");
            continue;
        }
        if (contents->srcBegin != static_cast<std::uint64_t>(l1) || contents->srcEnd != static_cast<std::uint64_t>(o2)) {
            finishBad("/ByteRange does not exclude exactly the /Contents value");
            continue;
        }
        if (contents->text.size() > kMaxSignatureContentsBytes) {
            finishBad("signature /Contents is oversized");
            continue;
        }
        info.byteRangeValid = true;
        const std::uint64_t end = static_cast<std::uint64_t>(o2) + static_cast<std::uint64_t>(l2);
        info.signedRevisionEnd = end;
        {
            bool onlyWhitespace = true;
            for (std::uint64_t p = end; p < fileSize; ++p)
                if (!isPdfWhitespace(pdf[p])) {
                    onlyWhitespace = false;
                    break;
                }
            info.coversWholeFile = onlyWhitespace;
        }

        if (info.subFilter == "ETSI.RFC3161" || info.type == "DocTimeStamp") {
            v.crypto = CryptoCheck::Unsupported;
            v.integrity = IntegrityStatus::Unknown;
            v.notes.push_back("document timestamp signatures are not validated in this release");
            v.state = crypto::foldState(v);
            results.push_back(std::move(info));
            continue;
        }
        if (info.subFilter != "adbe.pkcs7.detached" && info.subFilter != "ETSI.CAdES.detached") {
            v.crypto = CryptoCheck::Unsupported;
            v.integrity = IntegrityStatus::Unknown;
            v.notes.push_back("unsupported /SubFilter " + (info.subFilter.empty() ? std::string("(none)") : info.subFilter));
            v.state = crypto::foldState(v);
            results.push_back(std::move(info));
            continue;
        }

        // ---- CMS
        crypto::Bytes der(contents->text.begin(), contents->text.end());
        if (const auto total = derTotalLength(der)) {
            const bool paddingZero = std::all_of(der.begin() + static_cast<std::ptrdiff_t>(*total), der.end(),
                                                 [](std::uint8_t b) { return b == 0; });
            if (!paddingZero) {
                finishBad("non-zero bytes after the CMS structure");
                continue;
            }
            der.resize(*total);
        } else {
            while (!der.empty() && der.back() == 0) der.pop_back();
        }
        auto model = crypto::parseCms(der);
        if (!model) {
            finishBad("malformed CMS: " + model.error().message);
            continue;
        }
        if (model->signers.size() != 1 || !model->isDetached()) {
            finishBad("CMS must be detached with exactly one signer");
            continue;
        }
        const crypto::ContentSource content = crypto::ContentSource::fromSpans(
            {ByteView(pdf.data(), static_cast<std::size_t>(l1)),
             ByteView(pdf.data() + o2, static_cast<std::size_t>(l2))});
        crypto::CmsVerifyOptions vo;
        vo.anchors = options.anchors;
        vo.validationTime = options.validationTime;
        vo.extraCertificates = options.extraCertificates;
        vo.cancel = options.cancel;
        auto sv = crypto::verifyCms(*model, provider, &content, vo);
        if (!sv) return failCrypto(sv.error());
        if (sv->empty()) {
            finishBad("CMS has no signer");
            continue;
        }
        const crypto::SignerVerification& s0 = (*sv)[0];
        info.signer = s0.signer;
        info.digest = s0.digest;
        info.hasTimestampToken = s0.hasTimestampToken;
        v = s0.validation;

        const crypto::CmsSignerInfo& si = model->signers[0];
        if (info.subFilter == "ETSI.CAdES.detached") {
            const bool essV2 = si.findSigned(crypto::oid::kSigningCertificateV2) != nullptr;
            const bool hasTime = si.findSigned(crypto::oid::kSigningTime) != nullptr;
            if (!essV2) v.notes.push_back("CAdES signature without signing-certificate-v2");
            if (hasTime) v.notes.push_back("signing-time attribute present: not PAdES B-B (EN 319 142-1)");
            info.padesBaselineB = essV2 && !hasTime && si.hasSignedAttrs() &&
                                  si.findSigned(crypto::oid::kMessageDigest) && si.findSigned(crypto::oid::kContentType) &&
                                  s0.digest && !crypto::isLegacy(*s0.digest) && v.crypto == CryptoCheck::Valid;
        }

        // ---- modification analysis of later revisions
        if (v.crypto == CryptoCheck::Valid && v.integrity == IntegrityStatus::Intact) {
            std::size_t k = f.sectionCount();
            for (std::size_t i = 0; i < f.sectionCount(); ++i) {
                const std::uint64_t eof = f.section(i).eofEnd;
                if (eof != 0 && eof <= end) {
                    k = i;
                    break;
                }
            }
            const auto signedEntry = k < f.sectionCount() ? f.lookup(fd.ref.num, k) : std::nullopt;
            if (k == f.sectionCount() || !signedEntry || signedEntry->type != XrefEntry::Type::InUse ||
                signedEntry->a != entry->a) {
                v.modification = ModificationStatus::Unknown;
                v.notes.push_back("signed bytes do not end at a complete revision");
            } else if (k == 0) {
                v.modification = info.coversWholeFile ? ModificationStatus::None : ModificationStatus::Unknown;
                if (!info.coversWholeFile) v.notes.push_back("data follows the last revision");
            } else {
                FieldLock lock = fd.fieldLock.value_or(FieldLock{});
                if (info.fieldMdp) lock = *info.fieldMdp;
                ModificationAnalyzer an(f, k, std::move(lock));
                auto r = an.run();
                v.modification = r.status;
                for (auto& n : r.notes) v.notes.push_back(std::move(n));
                if (r.status == ModificationStatus::None && !info.coversWholeFile) {
                    v.modification = ModificationStatus::Unknown;
                    v.notes.push_back("data follows the last revision");
                }
            }
        }
        v.state = crypto::foldState(v);
        // An unclassifiable later change must never read as a plain valid signature.
        if (v.modification == ModificationStatus::Unknown &&
            (v.state == crypto::SignatureState::ValidTrusted || v.state == crypto::SignatureState::ValidUntrusted ||
             v.state == crypto::SignatureState::ValidExpiredNow || v.state == crypto::SignatureState::ValidAtTrustedTimestamp ||
             v.state == crypto::SignatureState::ValidTimestamped || v.state == crypto::SignatureState::ValidTimestampInvalid))
            v.state = crypto::SignatureState::UnableToValidate;
        results.push_back(std::move(info));
    }
    return results;
}

std::string describeSignature(const PdfSignatureInfo& s) {
    std::ostringstream out;
    out << "signature " << s.index << ": field=" << (s.fieldName.empty() ? "(none)" : s.fieldName)
        << " subfilter=" << (s.subFilter.empty() ? "(none)" : s.subFilter) << "\n";
    out << "  state: " << crypto::toString(s.validation.state) << "\n";
    out << "  crypto=" << crypto::toString(s.validation.crypto)
        << " integrity=" << crypto::toString(s.validation.integrity)
        << " certificate=" << crypto::toString(s.validation.certificate) << " trust=" << crypto::toString(s.validation.trust)
        << " modification=" << crypto::toString(s.validation.modification)
        << " timestamp=" << crypto::toString(s.validation.timestamp)
        << " revocation=" << crypto::toString(s.validation.revocation)
        << " policy=" << crypto::toString(s.validation.policy) << "\n";
    out << "  byte range:";
    for (const auto v : s.byteRange) out << ' ' << v;
    out << (s.coversWholeFile ? " (covers the whole file)" : " (later revisions follow)") << "\n";
    if (s.certification) out << "  certification: DocMDP P=" << static_cast<int>(*s.certification) << "\n";
    if (s.fieldMdp) out << "  FieldMDP lock present\n";
    if (s.signer) out << "  signer: " << s.signer->subject << " (sha256 " << s.signer->sha256Fingerprint << ")\n";
    if (s.digest) out << "  digest: " << crypto::toString(*s.digest) << "\n";
    if (!s.signingTimeText.empty()) out << "  /M: " << s.signingTimeText << "\n";
    if (!s.reason.empty()) out << "  reason: " << s.reason << "\n";
    if (!s.location.empty()) out << "  location: " << s.location << "\n";
    if (s.subFilter == "ETSI.CAdES.detached") out << "  PAdES B-B container: " << (s.padesBaselineB ? "yes" : "no") << "\n";
    for (const std::string& n : s.validation.notes) out << "  note: " << n << "\n";
    return out.str();
}

} // namespace rivet::pdf::sign
