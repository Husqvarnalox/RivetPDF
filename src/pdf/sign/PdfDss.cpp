// SPDX-License-Identifier: MPL-2.0
#include "pdf/sign/PdfDss.hpp"

#include "pdf/sign/RevisionWriter.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <set>

namespace rivet::pdf::sign {

namespace {

constexpr std::size_t kMaxProblems = 12;

std::string hexUpper(ByteView data) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(data.size() * 2);
    for (const std::uint8_t b : data) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

std::string pdfDateUtc(std::chrono::system_clock::time_point t) {
    using namespace std::chrono;
    const auto secs = floor<seconds>(t);
    const auto days = floor<std::chrono::days>(secs);
    const year_month_day ymd{days};
    const hh_mm_ss<seconds> hms{secs - days};
    char buf[40];
    std::snprintf(buf, sizeof buf, "D:%04d%02u%02u%02d%02d%02d+00'00'", static_cast<int>(ymd.year()),
                  static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()),
                  static_cast<int>(hms.hours().count()), static_cast<int>(hms.minutes().count()),
                  static_cast<int>(hms.seconds().count()));
    return buf;
}

struct Reader {
    PdfFile& f;
    std::size_t view;
    DssData out;
    std::size_t totalBytes = 0;

    void problem(std::string s) {
        if (out.problems.size() < kMaxProblems) out.problems.push_back(std::move(s));
    }

    // Value of an array entry that is a direct array or a reference to one; nullopt for anything else.
    std::optional<PdfArray> arrayOf(const PdfObject& o) {
        const PdfObject r = f.resolve(o, view);
        if (!r.isArray()) return std::nullopt;
        return *r.array;
    }

    void readStreams(const PdfObject* arrayObj, std::vector<DssItem>& into, const char* what) {
        if (!arrayObj) return;
        auto arr = arrayOf(*arrayObj);
        if (!arr) {
            problem(std::string(what) + " is not an array");
            return;
        }
        if (arr->size() > kMaxDssItems) {
            problem(std::string(what) + " has too many entries; the excess is ignored");
        }
        std::size_t n = 0;
        for (const PdfObject& e : *arr) {
            if (++n > kMaxDssItems) break;
            if (!e.isRef()) {
                problem(std::string(what) + " holds a direct object instead of a stream reference");
                continue;
            }
            auto ind = f.loadIndirect(e.ref.num, view);
            if (!ind || !ind->isStream) {
                problem(std::string(what) + " entry " + std::to_string(e.ref.num) + " is not a readable stream");
                continue;
            }
            if (ind->streamLength > kMaxDssItemBytes || totalBytes + ind->streamLength > kMaxDssTotalBytes) {
                problem(std::string(what) + " entry " + std::to_string(e.ref.num) + " exceeds the size limit");
                continue;
            }
            auto data = f.decodeStream(*ind, kMaxDssItemBytes);
            if (!data) {
                problem(std::string(what) + " entry " + std::to_string(e.ref.num) + " cannot be decoded");
                continue;
            }
            totalBytes += data->size();
            into.push_back({e.ref, std::move(*data)});
        }
    }

    void readEvidence(const PdfDict& d, DssEvidence& into, const char* certKey, const char* ocspKey, const char* crlKey) {
        readStreams(d.find(certKey), into.certs, certKey);
        readStreams(d.find(ocspKey), into.ocsps, ocspKey);
        readStreams(d.find(crlKey), into.crls, crlKey);
    }

    void run() {
        auto cat = f.catalog(view);
        if (!cat) return;
        const PdfObject* dssObj = cat->second.find("DSS");
        if (!dssObj) return;
        const PdfObject dss = f.resolve(*dssObj, view);
        if (!dss.isDict()) {
            problem("/DSS is not a dictionary");
            return;
        }
        out.present = true;
        readEvidence(*dss.dict, out.global, "Certs", "OCSPs", "CRLs");
        const PdfObject* vriObj = dss.dict->find("VRI");
        if (!vriObj) return;
        const PdfObject vri = f.resolve(*vriObj, view);
        if (!vri.isDict()) {
            problem("/VRI is not a dictionary");
            return;
        }
        std::size_t n = 0;
        for (const auto& [key, value] : vri.dict->entries) {
            if (++n > kMaxDssItems) {
                problem("too many /VRI entries; the excess is ignored");
                break;
            }
            const PdfObject entry = f.resolve(value, view);
            if (!entry.isDict()) {
                problem("/VRI entry " + key + " is not a dictionary");
                continue;
            }
            VriEntry e;
            e.key = key;
            readEvidence(*entry.dict, e.evidence, "Cert", "OCSP", "CRL");
            if (const PdfObject* tu = entry.dict->find("TU"); tu && tu->isString()) e.tu = tu->text;
            e.hasTs = entry.dict->find("TS") != nullptr;
            out.vri.emplace(key, std::move(e));
        }
    }
};

} // namespace

DssData readDss(PdfFile& file, std::size_t view) {
    Reader r{file, view, {}, 0};
    r.run();
    return std::move(r.out);
}

SignResult<std::string> vriKey(crypto::CryptoProvider& provider, ByteView contentsDer) {
    auto d = provider.digest(crypto::DigestAlgorithm::Sha1, contentsDer);
    if (!d) return failCrypto(d.error());
    return hexUpper(*d);
}

namespace {

struct Writer {
    PdfFile& f;
    crypto::CryptoProvider& provider;
    RevisionWriter rw;
    std::map<std::string, PdfRef> byHash; // SHA-256 hex -> existing or new stream
    std::size_t newStreams = 0, reused = 0;

    SignResult<PdfRef> stream(const Bytes& der) {
        auto h = provider.digest(crypto::DigestAlgorithm::Sha256, der);
        if (!h) return failCrypto(h.error());
        const std::string key = hexUpper(*h);
        if (auto it = byHash.find(key); it != byHash.end()) {
            ++reused;
            return it->second;
        }
        auto num = rw.allocate();
        if (!num) return std::unexpected(num.error());
        rw.add(streamObject(*num, std::string(der.begin(), der.end())));
        ++newStreams;
        return byHash[key] = PdfRef{*num, 0};
    }
};

// Existing array entries of `d[key]` (direct, or an indirect array object that stays in place and is flattened).
SignResult<PdfArray> existingArray(PdfFile& f, const PdfDict& d, const char* key) {
    const PdfObject* o = d.find(key);
    if (!o || o->isNull()) return PdfArray{};
    if (o->isArray()) return *o->array;
    if (o->isRef()) {
        auto obj = f.loadObject(o->ref.num);
        if (obj && *obj && (*obj)->isArray()) return *(*obj)->array;
    }
    return fail(SignErrorCode::UnsupportedStructure, std::string("the existing /DSS entry /") + key + " is not an array; it cannot be extended");
}

// Appends refs not yet present; returns whether anything was added.
bool mergeRefs(PdfArray& array, const std::vector<PdfRef>& refs) {
    std::set<PdfRef> present;
    for (const PdfObject& e : array)
        if (e.isRef()) present.insert(e.ref);
    bool added = false;
    for (const PdfRef& r : refs)
        if (present.insert(r).second) {
            array.push_back(PdfObject::makeRef(r));
            added = true;
        }
    return added;
}

SignResult<std::vector<PdfRef>> streamRefs(Writer& w, const std::vector<Bytes>& items) {
    std::vector<PdfRef> out;
    for (const Bytes& b : items) {
        auto r = w.stream(b);
        if (!r) return std::unexpected(r.error());
        out.push_back(*r);
    }
    return out;
}

void indexExisting(Writer& w, const DssEvidence& e) {
    for (const auto* list : {&e.certs, &e.ocsps, &e.crls})
        for (const DssItem& it : *list) {
            auto h = w.provider.digest(crypto::DigestAlgorithm::Sha256, it.data);
            if (h) w.byHash.emplace(hexUpper(*h), it.ref);
        }
}

} // namespace

SignResult<DssWriteResult> appendDss(Bytes pdf, const DssAdditions& add, crypto::CryptoProvider& provider) {
    if (pdf.size() > kMaxSignableFileSize - (std::uint64_t{16} << 20))
        return fail(SignErrorCode::LimitExceeded, "file too large (limit 2 GiB)");
    DssWriteResult result;

    std::optional<Writer> writer;
    PdfRef catalogRef;
    PdfDict catalog;
    std::optional<PdfRef> dssRef;
    PdfDict dss;
    std::optional<PdfRef> vriRef;
    PdfDict vri;
    bool headerPdf2 = false;
    {
        auto opened = PdfFile::open(pdf);
        if (!opened) return std::unexpected(opened.error());
        PdfFile& f = *opened;
        if (f.isEncrypted())
            return fail(SignErrorCode::EncryptedDocument, "the document is encrypted; validation data is not added (ADR-0037)");
        auto cat = f.catalog();
        if (!cat) return std::unexpected(cat.error());
        catalogRef = cat->first;
        catalog = cat->second;
        headerPdf2 = !f.headerVersion().empty() && f.headerVersion()[0] == '2';
        if (auto ind = f.loadIndirect(catalogRef.num); ind && ind->isStream)
            return fail(SignErrorCode::UnsupportedStructure, "the catalog is a stream object and cannot be rewritten");

        auto rw = RevisionWriter::begin(f);
        if (!rw) return std::unexpected(rw.error());
        writer.emplace(Writer{f, provider, std::move(*rw), {}, 0, 0});
        Writer& w = *writer;

        if (const PdfObject* d = catalog.find("DSS"); d && !d->isNull()) {
            if (d->isRef()) {
                auto obj = f.loadObject(d->ref.num);
                if (!obj) return std::unexpected(obj.error());
                if (!*obj || !(*obj)->isDict()) return fail(SignErrorCode::UnsupportedStructure, "/DSS is not a dictionary");
                if (auto ind = f.loadIndirect(d->ref.num); ind && ind->isStream)
                    return fail(SignErrorCode::UnsupportedStructure, "/DSS is a stream");
                dss = *(*obj)->dict;
                dssRef = d->ref;
            } else if (d->isDict()) {
                dss = *d->dict;
            } else {
                return fail(SignErrorCode::UnsupportedStructure, "/DSS is not a dictionary; it cannot be extended");
            }
        }
        if (const PdfObject* v = dss.find("VRI"); v && !v->isNull()) {
            if (v->isRef()) {
                auto obj = f.loadObject(v->ref.num);
                if (!obj || !*obj || !(*obj)->isDict())
                    return fail(SignErrorCode::UnsupportedStructure, "/DSS /VRI is not a dictionary");
                if (auto ind = f.loadIndirect(v->ref.num); ind && ind->isStream)
                    return fail(SignErrorCode::UnsupportedStructure, "/VRI is a stream");
                vri = *(*obj)->dict;
                vriRef = v->ref;
            } else if (v->isDict()) {
                vri = *v->dict;
            } else {
                return fail(SignErrorCode::UnsupportedStructure, "/DSS /VRI is not a dictionary");
            }
        }
        const DssData existing = readDss(f, 0);
        indexExisting(w, existing.global);
        for (const auto& [k, e] : existing.vri) indexExisting(w, e.evidence);

        bool changed = false;
        // ---- global arrays
        static constexpr const char* kGlobalKeys[3] = {"Certs", "OCSPs", "CRLs"};
        static constexpr const char* kVriKeys[3] = {"Cert", "OCSP", "CRL"};
        auto listOf = [](const DssEvidenceBytes& e, int i) -> const std::vector<Bytes>& {
            return i == 0 ? e.certs : i == 1 ? e.ocsps : e.crls;
        };
        std::array<PdfArray, 3> globalArrays;
        for (int i = 0; i < 3; ++i) {
            auto arr = existingArray(f, dss, kGlobalKeys[i]);
            if (!arr) return std::unexpected(arr.error());
            globalArrays[static_cast<std::size_t>(i)] = std::move(*arr);
        }
        auto addGlobal = [&](int i, const std::vector<PdfRef>& refs) {
            if (mergeRefs(globalArrays[static_cast<std::size_t>(i)], refs)) changed = true;
        };
        for (int i = 0; i < 3; ++i) {
            auto refs = streamRefs(w, listOf(add.global, i));
            if (!refs) return std::unexpected(refs.error());
            addGlobal(i, *refs);
        }

        // ---- VRI entries
        for (const VriAddition& va : add.vri) {
            auto key = vriKey(provider, va.signatureContents);
            if (!key) return std::unexpected(key.error());
            std::array<std::vector<PdfRef>, 3> refs;
            for (int i = 0; i < 3; ++i) {
                auto r = streamRefs(w, listOf(va.evidence, i));
                if (!r) return std::unexpected(r.error());
                refs[static_cast<std::size_t>(i)] = std::move(*r);
                addGlobal(i, refs[static_cast<std::size_t>(i)]);
            }
            PdfDict entry;
            bool entryExisted = false;
            std::optional<PdfRef> entryRef;
            if (const PdfObject* e = vri.find(*key)) {
                const PdfObject r = f.resolve(*e, 0);
                if (!r.isDict()) return fail(SignErrorCode::UnsupportedStructure, "an existing /VRI entry is not a dictionary");
                entry = *r.dict;
                entryExisted = true;
                if (e->isRef()) entryRef = e->ref;
            }
            bool entryChanged = !entryExisted;
            for (int i = 0; i < 3; ++i) {
                auto arr = existingArray(f, entry, kVriKeys[i]);
                if (!arr) return std::unexpected(arr.error());
                if (mergeRefs(*arr, refs[static_cast<std::size_t>(i)])) entryChanged = true;
                if (!arr->empty()) entry.set(kVriKeys[i], PdfObject::makeArray(std::move(*arr)));
            }
            if (!entryChanged) continue;
            changed = true;
            entry.set("TU", PdfObject::makeString(pdfDateUtc(va.tu)));
            if (entryRef) {
                w.rw.add(plainObject(entryRef->num, generationOf(f, entryRef->num), PdfObject::makeDict(std::move(entry))));
            } else {
                vri.set(*key, PdfObject::makeDict(std::move(entry)));
            }
        }
        if (!changed) {
            // Everything is already present (deduplicated by SHA-256): append nothing.
            result.data = std::move(pdf);
            result.reusedStreams = w.reused;
            return result;
        }

        // ---- assemble
        for (int i = 0; i < 3; ++i)
            if (!globalArrays[static_cast<std::size_t>(i)].empty())
                dss.set(kGlobalKeys[i], PdfObject::makeArray(std::move(globalArrays[static_cast<std::size_t>(i)])));
        if (!dss.find("Type")) dss.set("Type", PdfObject::makeName("DSS"));
        if (!vri.entries.empty()) {
            if (vriRef) {
                w.rw.add(plainObject(vriRef->num, generationOf(f, vriRef->num), PdfObject::makeDict(std::move(vri))));
            } else {
                dss.set("VRI", PdfObject::makeDict(std::move(vri)));
            }
        }
        bool catalogChanged = false;
        if (dssRef) {
            w.rw.add(plainObject(dssRef->num, generationOf(f, dssRef->num), PdfObject::makeDict(std::move(dss))));
        } else {
            auto num = w.rw.allocate();
            if (!num) return std::unexpected(num.error());
            w.rw.add(plainObject(*num, 0, PdfObject::makeDict(std::move(dss))));
            catalog.set("DSS", PdfObject::makeRef({*num, 0}));
            catalogChanged = true;
        }
        if (!headerPdf2) {
            // ETSI EN 319 142-1 / ISO 32000-1 developer extension (ADR-0038); idempotent.
            const PdfObject* ext = catalog.find("Extensions");
            const PdfObject esic = dictOf({{"BaseVersion", PdfObject::makeName("1.7")}, {"ExtensionLevel", PdfObject::makeInt(2)}});
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
        if (catalogChanged) w.rw.add(plainObject(catalogRef.num, catalogRef.gen, PdfObject::makeDict(std::move(catalog))));
        result.newStreams = w.newStreams;
        result.reusedStreams = w.reused;
    } // PdfFile closed: `pdf` may grow now

    if (auto r = writer->rw.appendTo(pdf); !r) return std::unexpected(r.error());
    result.changed = true;
    result.data = std::move(pdf);
    return result;
}

} // namespace rivet::pdf::sign
