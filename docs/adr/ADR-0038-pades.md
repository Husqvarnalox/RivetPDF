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
confirmed before external claims of conformance.

## Not in scope

B-T (signature timestamp), B-LT (`/DSS` with validation data), B-LTA (document
timestamp). The API supports them through `finalizeCms`, `extraReserveBytes`
and appending further incremental revisions. DocTimeStamp signatures are
reported unsupported by the verifier for now.
