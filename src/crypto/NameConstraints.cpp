// SPDX-License-Identifier: MPL-2.0
#include "crypto/NameConstraints.hpp"

#include "crypto/Der.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>

namespace rivet::crypto {

namespace {

using der::Element;

constexpr std::size_t kMaxNames = 4096; // per extension: hostile certificates stay bounded

enum class NameType : std::uint8_t { Dns, Email, Directory, Ip, Uri, Other };

struct Name {
    NameType type = NameType::Other;
    std::uint8_t tag = 0; // the GeneralName tag, to tell unevaluable types apart
    Bytes value; // IA5 text, Name DER (directory), address octets (ip)
};

struct Subtrees {
    std::vector<Name> permitted;
    std::vector<Name> excluded;
    bool hasPermitted = false;
};

std::string lower(ByteView v) {
    std::string s(v.begin(), v.end());
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string lower(const std::string& in) { return lower(ByteView(reinterpret_cast<const std::uint8_t*>(in.data()), in.size())); }

bool isIa5(ByteView v) {
    return std::all_of(v.begin(), v.end(), [](std::uint8_t b) { return b < 0x80; });
}

// GeneralName ::= CHOICE; tags: otherName [0], rfc822 [1], dNS [2], x400 [3], directory [4] (explicit), ediParty [5],
// URI [6], iPAddress [7], registeredID [8].
std::optional<Name> readGeneralName(const Element& e, bool& malformed) {
    Name n;
    n.tag = e.tag;
    switch (e.tag) {
    case 0x81: n.type = NameType::Email; break;
    case 0x82: n.type = NameType::Dns; break;
    case 0x86: n.type = NameType::Uri; break;
    case 0x87: n.type = NameType::Ip; break;
    case 0xA4: {
        n.type = NameType::Directory;
        auto inner = der::children(e);
        if (!inner || inner->size() != 1 || (*inner)[0].tag != der::kSequence) {
            malformed = true;
            return std::nullopt;
        }
        n.value.assign((*inner)[0].full.begin(), (*inner)[0].full.end());
        return n;
    }
    case 0xA0: case 0xA3: case 0xA5: case 0x88: n.type = NameType::Other; return n;
    default: malformed = true; return std::nullopt;
    }
    if ((n.type == NameType::Email || n.type == NameType::Dns || n.type == NameType::Uri) && !isIa5(e.content)) {
        malformed = true;
        return std::nullopt;
    }
    n.value.assign(e.content.begin(), e.content.end());
    return n;
}

struct ExtensionRef {
    Bytes value; // the extnValue content (DER of the extension's own structure)
};

// Walks the TBSCertificate extensions. Returns nullopt when absent; sets `malformed` on a parse failure.
std::optional<ExtensionRef> findExtension(ByteView certDer, std::string_view wantedOid, bool& malformed) {
    auto cert = der::readSingle(certDer);
    if (!cert || cert->tag != der::kSequence) { malformed = true; return std::nullopt; }
    auto top = der::children(*cert);
    if (!top || top->empty() || (*top)[0].tag != der::kSequence) { malformed = true; return std::nullopt; }
    auto tbs = der::children((*top)[0]);
    if (!tbs) { malformed = true; return std::nullopt; }
    for (const Element& f : *tbs) {
        if (f.tag != 0xA3) continue;
        auto wrap = der::children(f);
        if (!wrap || wrap->size() != 1 || (*wrap)[0].tag != der::kSequence) { malformed = true; return std::nullopt; }
        auto exts = der::children((*wrap)[0]);
        if (!exts) { malformed = true; return std::nullopt; }
        for (const Element& ext : *exts) {
            auto parts = der::children(ext);
            if (ext.tag != der::kSequence || !parts || parts->size() < 2 || parts->size() > 3 || (*parts)[0].tag != der::kOid) {
                malformed = true;
                return std::nullopt;
            }
            auto oid = der::decodeOid((*parts)[0].content);
            if (!oid) { malformed = true; return std::nullopt; }
            if (*oid != wantedOid) continue;
            auto raw = der::octetStringValue(parts->back());
            if (!raw) { malformed = true; return std::nullopt; }
            return ExtensionRef{std::move(*raw)};
        }
    }
    return std::nullopt;
}

bool parseSubtreeList(const Element& list, std::vector<Name>& out) {
    auto trees = der::children(list);
    if (!trees || trees->empty()) return false;
    for (const Element& tree : *trees) {
        auto parts = der::children(tree);
        if (tree.tag != der::kSequence || !parts || parts->empty()) return false;
        // minimum (default 0) / maximum (absent) must not be used (RFC 5280 4.2.1.10).
        if (parts->size() > 1) {
            for (std::size_t i = 1; i < parts->size(); ++i) {
                const Element& m = (*parts)[i];
                if (m.tag == 0x80) {
                    const Bytes n = der::normalizeInteger(m.content);
                    if (!(n.empty() || (n.size() == 1 && n[0] == 0))) return false;
                } else {
                    return false;
                }
            }
        }
        bool malformed = false;
        auto name = readGeneralName((*parts)[0], malformed);
        if (!name || out.size() >= kMaxNames) return false;
        out.push_back(std::move(*name));
    }
    return true;
}

// Returns false when the extension is malformed.
bool parseConstraints(ByteView extValue, Subtrees& out) {
    auto seq = der::readSingle(extValue);
    if (!seq || seq->tag != der::kSequence) return false;
    auto parts = der::children(*seq);
    if (!parts || parts->empty()) return false;
    for (const Element& p : *parts) {
        if (p.tag == 0xA0) {
            out.hasPermitted = true;
            if (!parseSubtreeList(p, out.permitted)) return false;
        } else if (p.tag == 0xA1) {
            if (!parseSubtreeList(p, out.excluded)) return false;
        } else {
            return false;
        }
    }
    return true;
}

// ---- matching ---------------------------------------------------------------------

bool dnsMatches(const std::string& constraint, const std::string& name) {
    if (constraint.empty()) return true;
    if (name.size() < constraint.size()) return false;
    if (name.compare(name.size() - constraint.size(), constraint.size(), constraint) != 0) return false;
    if (name.size() == constraint.size()) return constraint.front() != '.';
    // Label boundary: either the constraint starts with '.' or the preceding character is '.'.
    return constraint.front() == '.' || name[name.size() - constraint.size() - 1] == '.';
}

bool emailMatches(const std::string& constraint, const std::string& mailbox) {
    const auto at = mailbox.rfind('@');
    if (at == std::string::npos) return false;
    const std::string host = lower(mailbox.substr(at + 1));
    if (constraint.find('@') != std::string::npos) {
        const auto cat = constraint.rfind('@');
        return mailbox.substr(0, at) == constraint.substr(0, cat) && host == lower(constraint.substr(cat + 1));
    }
    const std::string c = lower(constraint);
    if (c.empty()) return true;
    if (c.front() == '.') return host.size() > c.size() && host.compare(host.size() - c.size(), c.size(), c) == 0;
    return host == c;
}

// Host of a URI authority, lower-cased; empty when there is none or it is an IP literal.
std::string uriHost(const std::string& uri) {
    const auto sch = uri.find("://");
    if (sch == std::string::npos) return {};
    std::string rest = uri.substr(sch + 3);
    rest = rest.substr(0, rest.find_first_of("/?#"));
    if (const auto at = rest.rfind('@'); at != std::string::npos) rest = rest.substr(at + 1);
    if (!rest.empty() && rest.front() == '[') return {};
    if (const auto colon = rest.find(':'); colon != std::string::npos) rest.resize(colon);
    return lower(rest);
}

bool uriMatches(const std::string& constraint, const std::string& uri) {
    const std::string host = uriHost(uri);
    if (host.empty()) return false;
    const std::string c = lower(constraint);
    if (c.empty()) return true;
    if (c.front() == '.') return host.size() > c.size() && host.compare(host.size() - c.size(), c.size(), c) == 0;
    return host == c;
}

bool ipMatches(ByteView constraint, ByteView addr) {
    if (addr.size() != 4 && addr.size() != 16) return false;
    if (constraint.size() != addr.size() * 2) return false;
    for (std::size_t i = 0; i < addr.size(); ++i)
        if ((addr[i] & constraint[addr.size() + i]) != (constraint[i] & constraint[addr.size() + i])) return false;
    return true;
}

struct Atv {
    std::string oid;
    std::uint8_t tag = 0;
    Bytes value;
};

// Canonical comparison form of a string attribute value (RFC 5280 7.1 simplified): ASCII case-folding and
// whitespace compression for the common string types, raw bytes otherwise.
std::string canonical(const Atv& a) {
    std::string raw(a.value.begin(), a.value.end());
    if (a.tag == 0x1E && raw.size() % 2 == 0) { // BMPString: keep ASCII-only values comparable
        std::string narrow;
        for (std::size_t i = 0; i + 1 < raw.size(); i += 2) {
            if (raw[i] != 0) return raw;
            narrow.push_back(raw[i + 1]);
        }
        raw = narrow;
    } else if (a.tag != 0x0C && a.tag != 0x13 && a.tag != 0x16 && a.tag != 0x14) {
        return raw;
    }
    std::string out;
    bool space = true;
    for (char c : raw) {
        if (c == ' ') {
            if (!space) out.push_back(' ');
            space = true;
        } else {
            out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
            space = false;
        }
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

using Rdn = std::vector<Atv>;

// Empty optional: malformed.
std::optional<std::vector<Rdn>> parseName(ByteView nameDer) {
    auto seq = der::readSingle(nameDer);
    if (!seq || seq->tag != der::kSequence) return std::nullopt;
    auto rdns = der::children(*seq);
    if (!rdns) return std::nullopt;
    std::vector<Rdn> out;
    for (const Element& set : *rdns) {
        auto atvs = der::children(set);
        if (set.tag != der::kSet || !atvs || atvs->empty()) return std::nullopt;
        Rdn rdn;
        for (const Element& atv : *atvs) {
            auto parts = der::children(atv);
            if (atv.tag != der::kSequence || !parts || parts->size() != 2 || (*parts)[0].tag != der::kOid) return std::nullopt;
            auto oid = der::decodeOid((*parts)[0].content);
            if (!oid) return std::nullopt;
            rdn.push_back({*oid, (*parts)[1].tag, Bytes((*parts)[1].content.begin(), (*parts)[1].content.end())});
        }
        out.push_back(std::move(rdn));
    }
    return out;
}

bool rdnEqual(const Rdn& a, const Rdn& b) {
    if (a.size() != b.size()) return false;
    for (const Atv& x : a) {
        const bool found = std::any_of(b.begin(), b.end(), [&](const Atv& y) { return x.oid == y.oid && canonical(x) == canonical(y); });
        if (!found) return false;
    }
    return true;
}

// The constraint DN is a prefix of the name's RDN sequence.
bool dnMatches(const std::vector<Rdn>& constraint, const std::vector<Rdn>& name) {
    if (constraint.size() > name.size()) return false;
    for (std::size_t i = 0; i < constraint.size(); ++i)
        if (!rdnEqual(constraint[i], name[i])) return false;
    return true;
}

constexpr const char* kEmailOid = "1.2.840.113549.1.9.1";

struct Evaluator {
    NameConstraintsResult result;

    void flag(NameConstraintsVerdict v, std::string d) {
        if (result.verdict == NameConstraintsVerdict::Ok) result = {v, std::move(d)};
    }

    bool subtreeMatches(const Name& tree, const Name& name) const {
        auto text = [](const Name& n) { return std::string(n.value.begin(), n.value.end()); };
        switch (name.type) {
        case NameType::Dns: return dnsMatches(lower(text(tree)), lower(text(name)));
        case NameType::Email: return emailMatches(text(tree), text(name));
        case NameType::Uri: return uriMatches(text(tree), text(name));
        case NameType::Ip: return ipMatches(tree.value, name.value);
        case NameType::Directory: {
            auto c = parseName(tree.value);
            auto n = parseName(name.value);
            return c && n && dnMatches(*c, *n);
        }
        case NameType::Other: return false;
        }
        return false;
    }

    void check(const Subtrees& st, const Name& name, std::string_view what) {
        if (name.type == NameType::Other) {
            // Unevaluable type: only a problem when some CA actually constrains that type.
            const auto sameTag = [&](const Name& t) { return t.type == NameType::Other && t.tag == name.tag; };
            if (std::any_of(st.excluded.begin(), st.excluded.end(), sameTag) || std::any_of(st.permitted.begin(), st.permitted.end(), sameTag))
                flag(NameConstraintsVerdict::Unsupported, std::string(what) + " has a name type that nameConstraints cannot be evaluated against");
            return;
        }
        for (const Name& ex : st.excluded)
            if (ex.type == name.type && subtreeMatches(ex, name)) {
                flag(NameConstraintsVerdict::Violation, std::string(what) + " falls inside an excluded subtree");
                return;
            }
        bool typeConstrained = false, matched = false;
        for (const Name& p : st.permitted) {
            if (p.type != name.type) continue;
            typeConstrained = true;
            if (subtreeMatches(p, name)) matched = true;
        }
        if (typeConstrained && !matched) flag(NameConstraintsVerdict::Violation, std::string(what) + " is outside every permitted subtree");
    }
};

struct CertNames {
    std::vector<Name> san;
    bool hasSan = false;
};

bool extractSan(const CertificateInfo& c, CertNames& out) {
    bool malformed = false;
    auto ext = findExtension(c.der, "2.5.29.17", malformed);
    if (malformed) return false;
    if (!ext) return true;
    out.hasSan = true;
    auto seq = der::readSingle(ext->value);
    if (!seq || seq->tag != der::kSequence) return false;
    auto items = der::children(*seq);
    if (!items || items->size() > kMaxNames) return false;
    for (const Element& item : *items) {
        bool bad = false;
        auto n = readGeneralName(item, bad);
        if (!n) return false;
        out.san.push_back(std::move(*n));
    }
    return true;
}

} // namespace

bool hasNameConstraintsExtension(ByteView certDer) {
    bool malformed = false;
    return findExtension(certDer, "2.5.29.30", malformed).has_value();
}

NameConstraintsResult checkNameConstraints(const std::vector<CertificateInfo>& chain) {
    Evaluator ev;
    // Constraining CAs, from the top: index i constrains every certificate j < i.
    for (std::size_t i = chain.size(); i-- > 1;) {
        bool malformed = false;
        auto ext = findExtension(chain[i].der, "2.5.29.30", malformed);
        if (malformed) {
            ev.flag(NameConstraintsVerdict::Malformed, "a CA certificate could not be parsed for nameConstraints");
            return ev.result;
        }
        if (!ext) continue;
        Subtrees st;
        if (!parseConstraints(ext->value, st)) {
            ev.flag(NameConstraintsVerdict::Malformed, "a CA certificate carries a malformed or unsupported nameConstraints extension");
            return ev.result;
        }
        for (std::size_t j = 0; j < i; ++j) {
            const CertificateInfo& c = chain[j];
            if (j != 0 && c.isSelfIssued()) continue; // RFC 5280 6.1.3(b)/(c)
            CertNames names;
            if (!extractSan(c, names)) {
                ev.flag(NameConstraintsVerdict::Malformed, "subjectAltName of a certificate in the path could not be parsed");
                return ev.result;
            }
            if (!c.subjectDer.empty()) {
                auto rdns = parseName(c.subjectDer);
                if (!rdns) {
                    ev.flag(NameConstraintsVerdict::Malformed, "the subject of a certificate in the path could not be parsed");
                    return ev.result;
                }
                if (!rdns->empty()) {
                    Name dn;
                    dn.type = NameType::Directory;
                    dn.value = c.subjectDer;
                    ev.check(st, dn, "the subject name");
                    for (const Rdn& rdn : *rdns)
                        for (const Atv& a : rdn)
                            if (a.oid == kEmailOid) {
                                Name mail;
                                mail.type = NameType::Email;
                                mail.value = a.value;
                                ev.check(st, mail, "the subject emailAddress");
                            }
                }
            }
            for (const Name& n : names.san) ev.check(st, n, "a subjectAltName entry");
            if (ev.result.verdict != NameConstraintsVerdict::Ok) return ev.result;
        }
    }
    return ev.result;
}

} // namespace rivet::crypto
