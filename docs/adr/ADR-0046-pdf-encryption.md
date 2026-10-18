# ADR-0046: PDF encryption (Set Password) with a post-save encryptor

Status: accepted

## Context

PDFium opens password-protected documents but cannot write them:
`FPDF_SaveAsCopy` never produces an `/Encrypt` dictionary for a new password.
Rivet needs "Set Password" without a second PDF backend and without a general
PDF parser (ADR-0003). The crypto primitives already live behind
`rivet_crypto` (OpenSSL provider, `RIVET_ENABLE_CRYPTO`).

## Decision

A constrained post-save encryptor, module `src/pdf/encrypt` (library
`rivet_pdf_encrypt`), API
`encryptPdf(bytes, EncryptOptions) -> core::Result<bytes>`. It takes the BYTES
of a PDFium save and returns the same document encrypted with the standard
security handler. It never touches PDFium (`FPDF_*` stays inside
`src/pdf/pdfium`) and is independent of `src/pdf/sign`.

### Supported output

- **AES-256**, V5/R6 (ISO 32000-2, `/CFM /AESV3`): default. File key from the
  crypto RNG, `/U /UE /O /OE /Perms` per the standard, Algorithm 2.B hash.
  Header is raised to `%PDF-2.0`.
- **AES-128**, V4/R4 (`/CFM /AESV2`): compatibility option. Header is raised to
  at least `%PDF-1.6`.
- User password (may be empty) and optional owner password. With no owner
  password a random one is generated and discarded: nobody can lift the
  permissions. Giving neither password and no restriction is refused.
- Permission bits: print, print high quality, modify, copy, annotate, fill
  forms, assemble, accessibility (reserved bits set as the standard requires;
  high-quality print is only set together with print).
- `EncryptMetadata`: when false the XMP `/Type /Metadata` stream stays plain and
  `/EncryptMetadata false` is written (and, for R4, folded into the key).
- Every string and stream gets its own random 16-byte IV (AES-CBC, PKCS#7);
  R4 derives the per-object key (Algorithm 1), R6 uses the file key directly.
- `/ID` is kept when well formed, otherwise minted from the RNG.

### Supported input (everything else is refused)

The scanner is a tokenizer-level reader of the format PDFium writes: one
classic cross-reference table, a trailer, indirect objects with strings and
`/Length`-framed streams (direct or indirect `/Length`). It is NOT a general
parser and does not interpret page content, fonts or images.

Refused with `ErrorCode::Unsupported` and a clear message:

- an existing `/Encrypt` (no re-encryption; remove the password first);
- **signed documents** (any `/ByteRange`). Encryption rewrites every byte, so it
  would invalidate the signature digest. Signing an already encrypted output is not supported either
  (the DSS writer refuses encrypted files, ADR-0045); that is a separate, later
  decision;
- object streams and cross-reference streams (`/ObjStm`, `/XRef`);
- incremental updates / hybrid files (`/Prev`, `/XRefStm`);
- streams with an explicit `/Crypt` filter.

Malformed input (bad offsets, wrong `/Length`, unterminated tokens, nesting
over 64) is `ErrorCode::InvalidDocument`. All loops are bounded and advance;
files over 2 GiB are refused.

### Passwords

- R6: UTF-8 after "SASLprep-lite": non-ASCII spaces map to U+0020, characters
  that map to nothing (soft hyphen, zero-width, variation selectors, BOM) are
  dropped, control characters are rejected; there is NO NFKC normalization.
  The result is truncated to 127 bytes on a code point boundary.
- R4: PDFDocEncoding approximated as Latin-1 (every code point below U+0100,
  at most 32 bytes); anything else is refused with advice to use AES-256.

### Primitives and their limits

- AES (CBC with PKCS#7, CBC without padding, ECB for `/Perms`) and MD5 are added
  to `rivet_crypto` (`crypto/Symmetric.hpp`, OpenSSL backend; the null build
  reports `BackendUnavailable`). MD5 is used only for R4 key derivation.
- RC4 is NOT used for content and no RC4 handler is written. The R4 `/O` and
  `/U` verification entries are defined by the standard with RC4; a 15-line RC4
  is implemented privately for those two 32-byte values only.
- No public-key security handlers, no V1/V2/V3 (RC4 / 40-bit) output, no
  Identity/Crypt filter selection.

### Build

`rivet_pdf_encrypt` builds in every configuration; with
`RIVET_ENABLE_CRYPTO=OFF` the null crypto provider makes `encryptPdf` return
`ErrorCode::NotAvailable`. The tests need the OpenSSL provider and are not built without it. The
`rivet-pdf-encrypt` dev tool reads passwords from
`RIVET_USER_PASSWORD` / `RIVET_OWNER_PASSWORD` or stdin, never argv.

## Verification

Unit tests re-derive keys from the written `/U /UE /O /OE /Perms` and decrypt
streams and strings independently of the writer. PDFium interop tests encrypt a
PDFium-saved document for {AES-256, AES-128} x {user, user+owner, owner only} x
{metadata encrypted, not encrypted} and reopen it through the engine with the
user and owner password: page count, extracted text and rendered pixels must
equal the original; wrong or missing passwords fail with `PasswordRequired`.
poppler (`pdfinfo`, `pdftotext`) is used as an independent reader when present.

## Consequences

- "Set Password" is a save-time transform of PDFium output; the UI (later) must
  save with PDFium first and offer the transform only for unsigned documents.
- The editor cannot keep editing an encrypted copy without the password in
  Rivet's own session; PDFium reopens it with the user or owner password.
- Digital signing of an encrypted output is out of scope here.
