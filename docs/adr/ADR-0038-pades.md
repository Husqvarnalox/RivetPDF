# ADR-0038: PAdES baseline B-B profile

Status: accepted

## Standards

ETSI EN 319 142-1 (PAdES digital signatures, Part 1: building blocks and
PAdES baseline signatures), ETSI EN 319 122-1 (CAdES baseline, referenced for
the CMS attributes), ISO 32000-1/2.

## Decision

`SignatureProfile::PadesBaselineB` produces:

- `/SubFilter /ETSI.CAdES.detached`;
- CMS with `contentType`, `messageDigest` and ESS `signing-certificate-v2`
  (binding the signer certificate by hash), digest SHA-256/384/512;
- no `signing-time` signed attribute (the `/M` entry carries the claimed time);
- signer certificate included, chain included by default;
- `/Extensions /ESIC` developer extension on PDF 1.x catalogs.

The verifier sets `padesBaselineB` only if the CAdES SubFilter, ESS-v2
(hash verified), no signing-time, messageDigest/contentType, a non-legacy
digest and a valid crypto check all hold. The flag describes the container
only, not trust. B-B is claimed only when these are met.

`Pkcs7Detached` (`adbe.pkcs7.detached`, signing-time included) is the basic
profile and is never labelled PAdES.

## Open item

The ESIC `/ExtensionLevel` is written as 2, following common implementations;
this value has not been checked against the ISO/ETSI registry text and must be
confirmed before external claims of conformance. The verifier only checks that the declaration is present when a `/DSS` exists; EN 319 102-1 conformance verification is not implemented.

## Not in scope

Nothing of the PAdES baseline levels: B-B, B-T, B-LT and B-LTA are all
produced and verified. B-T and document time-stamps are described in
"Timestamps" below; `/DSS`, `/VRI` and B-LTA in ADR-0045.

## Timestamps

**B-T.** `signPdf` takes an optional `SignatureTimestampOptions` with an
injectable `TimestampFetcher` (the PDF layer stays network-free; tests and the
CLI adapt `crypto::TimeStampClient`, which goes through `rivet::net`, ADR-0043).
After the CMS is signed, the hash of the signerInfo signature value is sent to
the TSA and the verified token is attached as the `id-aa-signatureTimeStampToken`
unsigned attribute, then the final CMS is written into the reserved `/Contents`.
The reservation is sized up front with a token estimate (default 8 KiB); if the
DER does not fit, signing returns `ContentsPlaceholderTooSmall` (with
`requiredBytes`) and nothing is written, after one automatic retry with a larger
reservation. If the TSA fails, signing fails with `TimestampFailed` and no file
is produced, unless `allowSignWithoutTimestamp` is set: the result is then B-B
and `timestampSkippedReason` says why. `SignedPdf::level` reports B-B or B-T.
B-T here means: ETSI.CAdES.detached container meeting B-B plus a signature
time-stamp whose imprint matches and whose TSA chain verifies against the
caller's anchors (`VerifyOptions::timestampAnchors`, falling back to `anchors`);
`PdfSignatureInfo::padesBaselineT` is set only then, and the state is
`ValidTimestamped`. A TSA certificate is never trusted from the OS store.

**DocTimeStamp.** `timestampPdf` appends an incremental revision with a
`/Type /DocTimeStamp /Filter /Adobe.PPKLite /SubFilter /ETSI.RFC3161` dictionary
whose `/Contents` is the raw token DER (hex, zero padded) and whose `/ByteRange`
covers the whole file; the TSA imprint is the hash of the ByteRange bytes. It
reuses the signer's placeholder and `ByteRange` machinery and the same
restrictions (xref table or uncompressed xref stream, 2 GiB limit, encrypted
files refused). It may follow any signature; a certification signature must
stay first, so none can be added after a DocTimeStamp. The verifier reports it as
`SignatureKind::DocumentTimestamp` with genTime and TSA subject, never as a
signature by a person. A DocTimeStamp-only later revision is not tampering for
earlier signatures (reads as no modification, even under DocMDP).

**Interop (recorded run, poppler 26.07 / OpenSSL 4.0.3).** `pdfsig` validates
the B-T signature as "Signature is Valid" and lists the DocTimeStamp as
Signature #2 with correct signed ranges but cannot interpret it ("Signature has
not yet been verified", type unknown); it does not report signature
time-stamps. `openssl ts -verify -token_in` accepts the extracted DocTimeStamp
token against the ByteRange digest and rejects a wrong digest.

**CLI.** `rivet-pdf-sign --timestamp-url U --timestamp-anchor F` (B-T) and
`rivet-pdf-sign timestamp ...` (DocTimeStamp) use `NetworkClient` with the
online-validation gate On: passing the URL is the user's consent. Without a
network transport the command reports `BackendUnavailable`.

A DocTimeStamp is a building block for B-LTA but is not by itself B-LTA: B-LT
and B-LTA (the `/DSS`, the `/VRI` entries and a document time-stamp covering the
validation data) are described in ADR-0045.
