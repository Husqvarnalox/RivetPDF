// SPDX-License-Identifier: MPL-2.0
#include "pdf/sign/PdfLtv.hpp"

#include "pdf/sign/LtvChain.hpp"
#include "pdf/sign/PdfSignatureVerifier.hpp"

#include <algorithm>
#include <fstream>

namespace rivet::pdf::sign {

std::string_view toString(PadesLevel level) {
    switch (level) {
    case PadesLevel::NotPades: return "not PAdES";
    case PadesLevel::BaselineB: return "B-B";
    case PadesLevel::BaselineT: return "B-T";
    case PadesLevel::BaselineLT: return "B-LT";
    case PadesLevel::BaselineLTA: return "B-LTA";
    }
    return "?";
}

namespace {

constexpr std::size_t kMaxListed = 24;

// Consults several stores as one.
class CompositeStore final : public crypto::EvidenceStore {
public:
    void add(std::shared_ptr<const crypto::EvidenceStore> s) {
        if (s) parts_.push_back(std::move(s));
    }
    std::vector<Bytes> ocspResponses() const override {
        std::vector<Bytes> out;
        for (const auto& p : parts_)
            for (Bytes& b : p->ocspResponses()) out.push_back(std::move(b));
        return out;
    }
    std::vector<Bytes> crls() const override {
        std::vector<Bytes> out;
        for (const auto& p : parts_)
            for (Bytes& b : p->crls()) out.push_back(std::move(b));
        return out;
    }

private:
    std::vector<std::shared_ptr<const crypto::EvidenceStore>> parts_;
};

void pushUnique(std::vector<Bytes>& v, const Bytes& b) {
    if (std::find(v.begin(), v.end(), b) == v.end()) v.push_back(b);
}

void mergeUnique(DssEvidenceBytes& into, const DssEvidenceBytes& from) {
    for (const Bytes& b : from.certs) pushUnique(into.certs, b);
    for (const Bytes& b : from.ocsps) pushUnique(into.ocsps, b);
    for (const Bytes& b : from.crls) pushUnique(into.crls, b);
}

void note(std::vector<std::string>& v, std::string s) {
    if (v.size() < kMaxListed && std::find(v.begin(), v.end(), s) == v.end()) v.push_back(std::move(s));
}

} // namespace

LtvCollector::LtvCollector(crypto::CryptoProvider& provider, LtvOptions options)
    : provider_(provider), options_(std::move(options)), known_(std::make_shared<crypto::VectorEvidenceStore>()) {
    if (options_.network && options_.fetchMissingIssuers) {
        aia_ = std::make_shared<crypto::AiaFetchLog>();
        crypto::AiaFetchOptions ao;
        ao.cancel = options_.cancel;
        fetcher_ = crypto::makeAiaFetcher(options_.network, ltv::borrowProvider(provider_), ao, aia_);
    }
}

void LtvCollector::seed(const DssData& existing) {
    auto take = [&](const DssEvidence& e) {
        for (const DssItem& o : e.ocsps) known_->addOcsp(o.data);
        for (const DssItem& c : e.crls) known_->addCrl(c.data);
        for (const DssItem& c : e.certs) knownCerts_.push_back(c.data);
    };
    take(existing.global);
    for (const auto& [k, v] : existing.vri) take(v.evidence);
}

LtvEvidence LtvCollector::collect(const LtvSubject& s) {
    LtvEvidence out;
    out.hasSigner = s.signer.has_value();
    const std::size_t aiaNotesBefore = aia_ ? aia_->notes.size() : 0;

    crypto::RevocationPolicy policy = options_.revocation;
    if (!options_.network) policy.allowNetwork = false;
    auto composite = std::make_shared<CompositeStore>();
    composite->add(known_);
    composite->add(options_.revocation.store);
    policy.store = composite;
    const crypto::TimePoint now = policy.now ? *policy.now : std::chrono::system_clock::now();

    std::vector<Bytes> poolDer = s.cmsCertificates;
    for (const Bytes& b : s.tokenCertificates) poolDer.push_back(b);
    for (const Bytes& b : knownCerts_) poolDer.push_back(b);
    for (const Bytes& b : options_.extraCertificates) poolDer.push_back(b);
    const auto pool = ltv::parseCertificates(provider_, poolDer);

    auto absorb = [&](const ltv::ChainEvidence& ce) {
        for (const crypto::CertificateInfo& c : ce.path.chain) pushUnique(out.evidence.certs, c.der);
        for (const Bytes& b : ce.ocsps) pushUnique(out.evidence.ocsps, b);
        for (const Bytes& b : ce.crls) pushUnique(out.evidence.crls, b);
        for (const std::string& m : ce.missing) note(out.missing, m);
        for (const std::string& m : ce.path.limitations) note(out.notes, m);
        for (const std::string& m : ce.path.weakNotes) note(out.notes, m);
    };

    if (s.signer) {
        ltv::ChainInput in;
        in.role = "signer";
        in.leaf = *s.signer;
        in.anchors = options_.anchors;
        in.pool = pool;
        in.fetchIssuers = fetcher_;
        in.validationTime = s.validationTime;
        in.now = now;
        const ltv::ChainEvidence ce = ltv::checkChain(provider_, in, options_.network, policy, options_.cancel);
        out.signerComplete = ce.complete;
        absorb(ce);
    }

    const crypto::TrustAnchorSet* tsaAnchors = options_.timestampAnchors ? options_.timestampAnchors : options_.anchors;
    ltv::ChainEvidence best;
    bool any = false;
    const auto candidates = ltv::parseCertificates(provider_, s.tokenCertificates);
    for (int pass = 0; pass < 2 && !any; ++pass) {
        for (const crypto::CertificateInfo& cand : candidates) {
            if (pass == 0 && !s.tsaSubject.empty() && cand.subject != s.tsaSubject) continue;
            ltv::ChainInput in;
            in.role = "time-stamp authority";
            in.leaf = cand;
            in.anchors = tsaAnchors;
            in.pool = pool;
            in.fetchIssuers = fetcher_;
            in.validationTime = s.validationTime;
            in.now = now;
            ltv::ChainEvidence ce = ltv::checkChain(provider_, in, options_.network, policy, options_.cancel);
            if (!any || ce.complete) best = std::move(ce);
            any = true;
            if (best.complete) break;
        }
    }
    if (!any) note(out.missing, "the time-stamp token carries no certificate of the time-stamp authority");
    out.timestampComplete = any && best.complete;
    if (any) absorb(best);

    if (aia_)
        for (std::size_t i = aiaNotesBefore; i < aia_->notes.size(); ++i) note(out.notes, "AIA: " + aia_->notes[i]);
    return out;
}

namespace {

struct Outcome {
    Bytes data;
    PadesLevel level = PadesLevel::NotPades;
    std::string downgrade;
    std::optional<PdfRef> signatureRef;
    std::size_t certs = 0, ocsps = 0, crls = 0, newStreams = 0, reusedStreams = 0;
    bool archived = false;
    bool timestamped = false;
    std::vector<std::string> notes;
};

SignError evidenceError(const std::vector<std::string>& missing) {
    SignError e{SignErrorCode::EvidenceUnavailable,
                "mandatory validation evidence is unavailable (" + std::to_string(missing.size()) + " problem(s))",
                std::nullopt};
    e.missingEvidence = missing;
    if (!missing.empty()) e.message += ": " + missing.front();
    return e;
}

VerifyOptions verifyOptionsFor(const LtvOptions& l) {
    VerifyOptions vo;
    vo.anchors = l.anchors;
    vo.timestampAnchors = l.timestampAnchors;
    vo.extraCertificates = l.extraCertificates;
    vo.validationTime = l.revocation.now ? *l.revocation.now : std::chrono::system_clock::now();
    vo.evaluateLtv = true;
    vo.requireVri = true;
    vo.cancel = l.cancel;
    return vo;
}

bool isCandidateKind(const PdfSignatureInfo& s) { return s.kind == SignatureKind::Signature; }

bool verifies(const PdfSignatureInfo& s) {
    return s.byteRangeValid && s.validation.crypto == crypto::CryptoCheck::Valid &&
           s.validation.integrity == crypto::IntegrityStatus::Intact;
}

std::string joinReasons(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& s : v) {
        if (!out.empty()) out += "; ";
        out += s;
    }
    return out;
}

SignResult<Outcome> upgradeImpl(Bytes pdf, PadesLevel target, const LtvOptions& ltvOpt, const TimestampFetcher& archive,
                                crypto::DigestAlgorithm archiveDigest, std::size_t archiveExpected,
                                std::optional<PdfRef> only, crypto::CryptoProvider& provider) {
    if (ltvOpt.cancel.cancelled()) return fail(SignErrorCode::Cancelled, "cancelled");
    if (target == PadesLevel::NotPades || target == PadesLevel::BaselineB)
        return fail(SignErrorCode::InvalidArgument, "an upgrade needs a target of B-T, B-LT or B-LTA");
    if (!ltvOpt.anchors && target >= PadesLevel::BaselineLT)
        return fail(SignErrorCode::InvalidArgument, "trust anchors are required to collect validation data");
    if (target >= PadesLevel::BaselineLT && ltvOpt.revocation.mode == crypto::RevocationMode::Off)
        return fail(SignErrorCode::InvalidArgument,
                    "revocation checking is off: B-LT / B-LTA cannot be produced without revocation evidence");
    if (target == PadesLevel::BaselineLTA && !archive)
        return fail(SignErrorCode::InvalidArgument, "B-LTA needs a time-stamp source for the archival document time-stamp");
    if (pdf.size() > kMaxSignableFileSize - (std::uint64_t{16} << 20))
        return fail(SignErrorCode::LimitExceeded, "file too large (limit 2 GiB)");

    const VerifyOptions vo = verifyOptionsFor(ltvOpt);
    auto before = verifyPdfSignatures(pdf, provider, vo);
    if (!before) return std::unexpected(before.error());

    Outcome out;
    std::vector<const PdfSignatureInfo*> cands;
    std::vector<std::string> skipped;
    bool sawB = false;
    for (const PdfSignatureInfo& s : *before) {
        if (!isCandidateKind(s)) continue;
        if (only && !(s.signatureRef && *s.signatureRef == *only)) continue;
        const std::string name = s.fieldName.empty() ? "signature " + std::to_string(s.index) : "'" + s.fieldName + "'";
        if (s.level == PadesLevel::NotPades || s.level == PadesLevel::BaselineB) {
            sawB = true;
            skipped.push_back(name + " is " + std::string(toString(s.level)) +
                              ": it has no signature time-stamp, which can only be requested while signing");
            continue;
        }
        if (!verifies(s) || !s.signer || !s.timestamp || !s.timestamp->genTime) {
            skipped.push_back(name + " does not verify and cannot be upgraded");
            continue;
        }
        cands.push_back(&s);
    }
    if (cands.empty()) {
        if (sawB)
            return fail(SignErrorCode::UnsupportedUpgrade,
                        "B-B to B-T is not possible after signing: a signature time-stamp must be requested while the "
                        "signature is created (" + joinReasons(skipped) + ")");
        return fail(SignErrorCode::UnsupportedUpgrade,
                    skipped.empty() ? "the document has no CAdES signature to upgrade" : joinReasons(skipped));
    }
    out.notes = skipped;

    std::vector<std::string> allMissing;
    if (target >= PadesLevel::BaselineLT) {
        LtvCollector collector(provider, ltvOpt);
        {
            auto opened = PdfFile::open(pdf);
            if (!opened) return std::unexpected(opened.error());
            collector.seed(readDss(*opened, 0));
        }

        DssAdditions add;
        for (const PdfSignatureInfo* s : cands) {
            if (s->level >= PadesLevel::BaselineLT) continue;
            LtvSubject subj;
            subj.signer = s->signer;
            subj.cmsCertificates = s->material.cmsCertificates;
            subj.tokenCertificates = s->material.tokenCertificates;
            subj.tsaSubject = s->timestamp->tsaSubject;
            subj.validationTime = *s->timestamp->genTime;
            LtvEvidence ev = collector.collect(subj);
            for (const std::string& m : ev.missing) note(allMissing, m);
            for (const std::string& m : ev.notes) note(out.notes, m);
            if (s->material.contentsDer.empty()) {
                note(allMissing, "the signature /Contents could not be read");
                continue;
            }
            mergeUnique(add.global, ev.evidence);
            VriAddition v;
            v.signatureContents = s->material.contentsDer;
            v.evidence = std::move(ev.evidence);
            add.vri.push_back(std::move(v));
        }
        if (!allMissing.empty() && !ltvOpt.allowPartial) return std::unexpected(evidenceError(allMissing));

        if (!add.vri.empty()) {
            out.certs = add.global.certs.size();
            out.ocsps = add.global.ocsps.size();
            out.crls = add.global.crls.size();
            auto w = appendDss(std::move(pdf), add, provider);
            if (!w) return std::unexpected(w.error());
            out.newStreams = w->newStreams;
            out.reusedStreams = w->reusedStreams;
            pdf = std::move(w->data);
        }
    }

    auto verifyNow = [&]() { return verifyPdfSignatures(pdf, provider, vo); };
    auto levelOf = [&](const std::vector<PdfSignatureInfo>& infos, std::vector<std::string>& reasons) {
        PadesLevel lowest = PadesLevel::BaselineLTA;
        bool seen = false;
        for (const PdfSignatureInfo& s : infos) {
            if (!isCandidateKind(s)) continue;
            const bool mine = std::any_of(cands.begin(), cands.end(), [&](const PdfSignatureInfo* c) {
                return c->signatureRef && s.signatureRef && *c->signatureRef == *s.signatureRef;
            });
            if (!mine) continue;
            seen = true;
            if (s.level < lowest) {
                lowest = s.level;
                reasons = s.ltv.reasons;
                for (const std::string& m : s.ltv.missing) note(reasons, m);
            }
        }
        return seen ? lowest : PadesLevel::NotPades;
    };

    auto mid = verifyNow();
    if (!mid) return fail(SignErrorCode::SelfVerifyFailed, "the upgraded file is unreadable: " + mid.error().message);
    std::vector<std::string> reasons;
    PadesLevel level = levelOf(*mid, reasons);

    if (target == PadesLevel::BaselineLTA && level >= PadesLevel::BaselineLT) {
        DocTimestampRequest dr;
        dr.fetch = archive;
        dr.digest = archiveDigest;
        dr.expectedTokenBytes = archiveExpected;
        dr.placement.kind = SignaturePlacement::Kind::Invisible;
        auto ts = timestampPdf(std::move(pdf), dr, provider);
        if (!ts) return std::unexpected(ts.error());
        pdf = std::move(ts->data);
        out.archived = true;
        out.signatureRef = ts->signatureRef;
        auto fin = verifyNow();
        if (!fin) return fail(SignErrorCode::SelfVerifyFailed, "the archived file is unreadable: " + fin.error().message);
        reasons.clear();
        level = levelOf(*fin, reasons);
    }

    out.level = level;
    out.timestamped = true;
    if (level < target) {
        std::string why = "the result is " + std::string(toString(level)) + ", not " + std::string(toString(target));
        if (!allMissing.empty()) why += ": " + joinReasons(allMissing);
        else if (!reasons.empty()) why += ": " + joinReasons(reasons);
        if (!ltvOpt.allowPartial) return fail(SignErrorCode::SelfVerifyFailed, why);
        out.downgrade = std::move(why);
    }
    if (!out.signatureRef) out.signatureRef = cands.front()->signatureRef;
    out.data = std::move(pdf);
    return out;
}

SignedPdf toSigned(Outcome&& o, std::size_t inputSize) {
    SignedPdf s;
    s.data = std::move(o.data);
    s.inputSize = inputSize;
    if (o.signatureRef) s.signatureRef = *o.signatureRef;
    s.level = o.level;
    s.timestamped = o.timestamped;
    s.levelDowngradeReason = std::move(o.downgrade);
    s.ltvOperation = true;
    s.dssCertificates = o.certs;
    s.dssOcspResponses = o.ocsps;
    s.dssCrls = o.crls;
    s.dssNewStreams = o.newStreams;
    s.dssReusedStreams = o.reusedStreams;
    s.archiveTimestamped = o.archived;
    s.ltvNotes = std::move(o.notes);
    return s;
}

} // namespace

SignResult<SignedPdf> upgradePdf(Bytes pdf, const UpgradeRequest& request, crypto::CryptoProvider& provider) {
    const std::size_t inputSize = pdf.size();
    auto r = upgradeImpl(std::move(pdf), request.target, request.ltv, request.archiveTimestamp, request.archiveDigest,
                         request.archiveExpectedTokenBytes, request.onlySignature, provider);
    if (!r) return std::unexpected(r.error());
    return toSigned(std::move(*r), inputSize);
}

SignResult<SignedPdf> addArchiveTimestamp(Bytes pdf, const ArchiveRenewalRequest& request, crypto::CryptoProvider& provider) {
    if (!request.fetch) return fail(SignErrorCode::InvalidArgument, "no time-stamp source supplied");
    if (!request.ltv.anchors && !request.ltv.timestampAnchors)
        return fail(SignErrorCode::InvalidArgument, "trust anchors are required to collect validation data");
    if (request.ltv.revocation.mode == crypto::RevocationMode::Off)
        return fail(SignErrorCode::InvalidArgument, "revocation checking is off: renewal needs revocation evidence");
    const std::size_t inputSize = pdf.size();
    const VerifyOptions vo = verifyOptionsFor(request.ltv);
    auto before = verifyPdfSignatures(pdf, provider, vo);
    if (!before) return std::unexpected(before.error());

    const PdfSignatureInfo* newest = nullptr;
    for (const PdfSignatureInfo& s : *before) {
        if (s.kind != SignatureKind::DocumentTimestamp || !verifies(s) || !s.timestamp || !s.timestamp->verified ||
            !s.timestamp->genTime)
            continue;
        if (!newest || (s.byteRange.size() == 4 && newest->byteRange.size() == 4 && s.byteRange[1] > newest->byteRange[1]))
            newest = &s;
    }
    if (!newest)
        return fail(SignErrorCode::UnsupportedUpgrade,
                    "the document has no valid archive time-stamp to renew; use upgradePdf to reach B-LTA first");

    std::vector<std::string> notes;
    LtvOptions lo = request.ltv;
    LtvCollector collector(provider, lo);
    {
        auto opened = PdfFile::open(pdf);
        if (!opened) return std::unexpected(opened.error());
        collector.seed(readDss(*opened, 0));
    }
    LtvSubject subj;
    subj.tokenCertificates = newest->material.tokenCertificates;
    subj.tsaSubject = newest->timestamp->tsaSubject;
    subj.validationTime = *newest->timestamp->genTime;
    LtvEvidence ev = collector.collect(subj);
    if (!ev.timestampComplete && !lo.allowPartial) return std::unexpected(evidenceError(ev.missing));
    notes = ev.notes;

    Outcome out;
    DssAdditions add;
    add.global = ev.evidence;
    VriAddition v;
    v.signatureContents = newest->material.contentsDer;
    v.evidence = ev.evidence;
    add.vri.push_back(std::move(v));
    out.certs = ev.evidence.certs.size();
    out.ocsps = ev.evidence.ocsps.size();
    out.crls = ev.evidence.crls.size();
    auto w = appendDss(std::move(pdf), add, provider);
    if (!w) return std::unexpected(w.error());
    out.newStreams = w->newStreams;
    out.reusedStreams = w->reusedStreams;

    DocTimestampRequest dr;
    dr.fetch = request.fetch;
    dr.digest = request.digest;
    dr.expectedTokenBytes = request.expectedTokenBytes;
    auto ts = timestampPdf(std::move(w->data), dr, provider);
    if (!ts) return std::unexpected(ts.error());
    out.data = std::move(ts->data);
    out.signatureRef = ts->signatureRef;
    out.archived = true;
    out.timestamped = true;
    out.notes = std::move(notes);
    out.notes.push_back("renewal covers the newest archive time-stamp's TSA chain only");

    auto fin = verifyPdfSignatures(out.data, provider, vo);
    if (!fin) return fail(SignErrorCode::SelfVerifyFailed, "the renewed file is unreadable: " + fin.error().message);
    PadesLevel lowest = PadesLevel::BaselineLTA;
    bool seen = false;
    std::vector<std::string> reasons;
    for (const PdfSignatureInfo& s : *fin) {
        if (s.kind != SignatureKind::Signature) continue;
        seen = true;
        if (s.level < lowest) {
            lowest = s.level;
            reasons = s.ltv.reasons;
        }
    }
    out.level = seen ? lowest : PadesLevel::NotPades;
    if (!ev.timestampComplete) out.downgrade = "the archive time-stamp's TSA chain evidence is incomplete: " + joinReasons(ev.missing);
    else if (out.level < PadesLevel::BaselineLTA && seen) out.downgrade = joinReasons(reasons);
    return toSigned(std::move(out), inputSize);
}

namespace detail {

SignResult<void> finishLtv(SignedPdf& signedPdf, PadesLevel target, const LtvOptions& ltv, const TimestampFetcher& archive,
                           crypto::DigestAlgorithm archiveDigest, crypto::CryptoProvider& provider) {
    const std::size_t inputSize = signedPdf.inputSize;
    auto r = upgradeImpl(std::move(signedPdf.data), target, ltv, archive, archiveDigest, 0, signedPdf.signatureRef, provider);
    if (!r) return std::unexpected(r.error());
    const PdfRef sigRef = signedPdf.signatureRef;
    SignedPdf merged = toSigned(std::move(*r), inputSize);
    merged.signatureRef = sigRef; // the signature that was created, not the archive time-stamp
    merged.fieldName = std::move(signedPdf.fieldName);
    merged.attempts = signedPdf.attempts;
    merged.reservedBytes = signedPdf.reservedBytes;
    merged.cmsBytes = signedPdf.cmsBytes;
    merged.classicXref = signedPdf.classicXref;
    merged.timestampTokenBytes = signedPdf.timestampTokenBytes;
    merged.timestampGenTime = signedPdf.timestampGenTime;
    merged.timestampTsaSubject = std::move(signedPdf.timestampTsaSubject);
    signedPdf = std::move(merged);
    return {};
}

} // namespace detail

SignResult<SignedPdf> upgradePdfFile(const std::filesystem::path& input, const std::filesystem::path& output,
                                     const UpgradeRequest& request, crypto::CryptoProvider& provider) {
    return detail::transactionalFile(input, output, provider,
                                           [&](Bytes data) { return upgradePdf(std::move(data), request, provider); });
}

SignResult<SignedPdf> addArchiveTimestampFile(const std::filesystem::path& input, const std::filesystem::path& output,
                                              const ArchiveRenewalRequest& request, crypto::CryptoProvider& provider) {
    return detail::transactionalFile(
        input, output, provider, [&](Bytes data) { return addArchiveTimestamp(std::move(data), request, provider); });
}

} // namespace rivet::pdf::sign
