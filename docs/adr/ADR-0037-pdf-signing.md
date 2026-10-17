# ADR-0037: PDF signing engine (incremental signer and verifier)

Status: accepted

## Standards

- ISO 32000-1:2008 (PDF 1.7) 12.8 (digital signatures), 12.7.4.5 (signature
  fields, `/Lock`), 7.5.6 (incremental updates), 7.5.8 (xref streams).
- ISO 32000-2:2020 12.8 (same structures; DocMDP/FieldMDP unchanged).
- RFC 5652 CMS, through `rivet_crypto` (ADR-0034/0035).

## Decisions

Module `src/pdf/sign` (`rivet_pdf_sign`) depends only on `rivet_core` and
`rivet_crypto`. It never calls PDFium and never rebuilds the document.

- **Incremental only.** The input bytes are an exact prefix of the output.
  One revision is appended: signature dictionary, field and widget, updated
  `/AcroForm` (`/SigFlags 3`), `/Annots`, catalog (`/Perms`, `/Extensions`),
  new xref section, trailer `/Prev`.
- **Xref style.** A table if the newest prior section is a table (a hybrid
  file counts as a table: the new section is a plain table whose `/Prev` points
  at the hybrid section, which keeps `/XRefStm` reachable); otherwise an
  uncompressed xref stream with `/W [1 4 2]`.
- **Placeholders.** `/ByteRange [0 <10> <10> <10>]` is fixed-width and
  space-padded, `/Contents` is a hex string of the reserved size. Files over
  2 GiB are refused (ByteRange values are 32-bit in common readers).
- **Digest over spans.** The digest covers three in-memory spans (prefix,
  revision head, revision tail) without copying. The revision is appended to the
  output only after the CMS fits. If the CMS does not fit, the revision is
  rebuilt with a larger reserve (at most 3 retries, 4 MiB cap); bytes after
  the digest are never shifted.
- **Transactional file flow.** `signPdfFile`: read, sign and self-verify in
  memory, write a temp file via `AtomicFileWriter`, read it back, check the
  input prefix hash and the signature, then atomically rename. Any failure
  leaves the source untouched.
- **Encrypted PDFs** are refused with `EncryptedDocument` (a signature over
  encrypted content needs the security handler, out of scope).
- **Certification.** `/Reference` DocMDP with `/TransformParams /P 1|2|3` and
  `/Perms /DocMDP`. Only valid as the first signature
  (`CertificationNotAllowed`); signing a P=1 document is refused
  (`ViolatesCertification`). FieldMDP lock: field `/Lock` plus a FieldMDP
  `/Reference` transform.
- **Visible appearance** is a minimal Helvetica, Latin-1 stream.

## Verifier

Every signature dictionary is untrusted. Rejected before any digest:
non-indirect or object-stream signature objects, duplicate keys, a ByteRange
that is not exactly four direct non-negative integers, `r0 != 0`, overlap,
out-of-bounds or overflowing values, `len2 == 0`; `/Contents` that is not hex
or whose token does not equal the gap exactly; non-zero bytes after the DER
structure; CMS over 8 MiB; a signature object referenced twice; more than one
signer; more than 4096 signatures. Unsupported SubFilters (including
DocTimeStamp) are reported `UnsupportedAlgorithm`/unsupported, never valid.

**Modification analysis** locates the signed revision (newest xref section ending
at or before the signed range) and compares later views object by object with
role-based classification (DSS, signature, form, annotation). Result is
`None`, `Permitted`, `Disallowed` or `Unknown`. Without DocMDP only signing and
DSS additions are permitted; form fill needs level >= 2, annotations level 3;
FieldMDP-locked fields always disallow; stream edits, deletions and page-tree
changes are `Disallowed`; anything unclassifiable, and any trailing non-
whitespace data, is `Unknown`. `Unknown` is never reported as a valid state.

Weak legacy digests (SHA-1) verify but are flagged `WeakLegacyAlgorithm`;
creation refuses them.

## Extension points

`SignRequest::finalizeCms` (unsigned attributes, e.g. a timestamp token) and
`extraReserveBytes` allow B-T/B-LT/B-LTA later; `/DSS` can be appended as another
incremental revision, which the analyzer already classifies as permitted.

## Tools and limits

`rivet-pdf-signature-inspect --pdf file.pdf [--trust cert]...` lists signatures
with their status. Not covered: encrypted documents, timestamps, `/DSS`
creation, revocation fetching, object-stream-compressed signature objects,
Unicode visible appearance text.
