#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
# Builds a corpus of Rivet-signed PDFs for manual Acrobat interop checks.
#
#   tools/qa/make-acrobat-corpus.sh [OUT_DIR] [BUILD_DIR]
#
# OUT_DIR   default /Users/a55532/Projects/rivet-acrobat-corpus (keep it outside
#           the repo; if you point it inside, it must be gitignored)
# BUILD_DIR default build/p-off (needs rivet-pdf-sign and rivet-pdf-signature-inspect,
#           configured with -DRIVET_ENABLE_CRYPTO=ON)
#
# Everything produced here is throwaway TEST material: self-signed identities
# with a published password. Never use them for anything real, never commit them.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-/Users/a55532/Projects/rivet-acrobat-corpus}"
BUILD="${2:-$ROOT/build/p-off}"
SIGN="$BUILD/src/pdf/sign/rivet-pdf-sign"
INSPECT="$BUILD/src/pdf/sign/rivet-pdf-signature-inspect"
HELPER="$ROOT/tools/qa/make_pdf_corpus_helper.py"
PASS="rivet-test"

[[ -x "$SIGN" && -x "$INSPECT" ]] || { echo "missing tools in $BUILD (build with crypto ON)" >&2; exit 1; }
case "$OUT" in "$ROOT"/*) grep -qxF "${OUT#"$ROOT"/}/" "$ROOT/.gitignore" || echo "warning: $OUT is inside the repo and not gitignored" >&2 ;; esac

rm -rf "$OUT"
mkdir -p "$OUT"/{src,identities,signed,logs}
export RIVET_P12_PASSWORD="$PASS"
PY() { python3 -I "$HELPER" "$@"; }

# --- source PDFs ----------------------------------------------------------
PY gen plain "$OUT/src/plain.pdf"
PY gen sigfield "$OUT/src/empty-sig-field.pdf"
PY gen xrefstream "$OUT/src/xref-stream.pdf"

# --- throwaway identities -------------------------------------------------
"$SIGN" --make-test-identity "$OUT/identities/rsa2048.p12" --alg rsa2048 --cn "RSA 2048"
"$SIGN" --make-test-identity "$OUT/identities/ecdsa-p256.p12" --alg ecp256 --cn "ECDSA P-256"
"$SIGN" --make-test-identity "$OUT/identities/rsa-pss.p12" --alg rsa-pss --cn "RSA-PSS"
for id in rsa2048 ecdsa-p256 rsa-pss; do
  openssl pkcs12 -in "$OUT/identities/$id.p12" -passin env:RIVET_P12_PASSWORD -nokeys -clcerts \
    -out "$OUT/identities/$id.crt.pem" 2>/dev/null || true
done
ID_RSA="$OUT/identities/rsa2048.p12"
ID_EC="$OUT/identities/ecdsa-p256.p12"
ID_PSS="$OUT/identities/rsa-pss.p12"

S="$OUT/signed"
sign() { "$SIGN" "$@" >>"$OUT/logs/sign.log" 2>&1; }

# already-signed source (used as input for a re-sign scenario)
sign --in "$OUT/src/plain.pdf" --out "$OUT/src/already-signed.pdf" --p12 "$ID_RSA" --profile basic --invisible --reason "first signer"

# --- Rivet-signed scenarios ----------------------------------------------
sign --in "$OUT/src/plain.pdf" --out "$S/01-visible-rsa.pdf" --p12 "$ID_RSA" --profile basic --visible 1,72,72,220,70 --reason "Corpus visible RSA" --location "Test"
sign --in "$OUT/src/plain.pdf" --out "$S/02-invisible-rsa.pdf" --p12 "$ID_RSA" --profile basic --invisible
sign --in "$OUT/src/plain.pdf" --out "$S/03-ecdsa-p256.pdf" --p12 "$ID_EC" --profile basic --alg ecdsa --visible 1,72,72,220,70
sign --in "$OUT/src/plain.pdf" --out "$S/04-rsa-pss.pdf" --p12 "$ID_PSS" --profile basic --alg rsa-pss --visible 1,72,72,220,70
sign --in "$OUT/src/plain.pdf" --out "$S/05-pades-b-b.pdf" --p12 "$ID_RSA" --profile pades-bb --visible 1,72,72,220,70
sign --in "$OUT/src/empty-sig-field.pdf" --out "$S/06-existing-field.pdf" --p12 "$ID_RSA" --profile basic --field SigField1
sign --in "$OUT/src/plain.pdf" --out "$S/07-certify-p2.pdf" --p12 "$ID_RSA" --profile basic --certify 2 --visible 1,72,72,220,70
sign --in "$OUT/src/plain.pdf" --out "$S/08a.tmp" --p12 "$ID_RSA" --profile basic --visible 1,72,72,220,70 --reason "first"
sign --in "$S/08a.tmp" --out "$S/08-two-sequential.pdf" --p12 "$ID_EC" --profile basic --alg ecdsa --visible 1,160,72,220,70 --reason "second"
rm -f "$S/08a.tmp"
sign --in "$OUT/src/xref-stream.pdf" --out "$S/09-xref-stream-source.pdf" --p12 "$ID_RSA" --profile basic --visible 1,72,72,220,70
sign --in "$OUT/src/already-signed.pdf" --out "$S/10-resign-already-signed.pdf" --p12 "$ID_EC" --profile basic --alg ecdsa --invisible --reason "second signer"
sign --in "$OUT/src/plain.pdf" --out "$S/11-sha512-rsa.pdf" --p12 "$ID_RSA" --profile basic --digest sha512 --visible 1,72,72,220,70

# --- post-sign mutations --------------------------------------------------
PY annot "$S/01-visible-rsa.pdf" "$S/20-mutation-annot-after-sign.pdf"
PY flip "$S/02-invisible-rsa.pdf" "$S/21-mutation-flipped-byte.pdf"
PY annot "$S/07-certify-p2.pdf" "$S/22-mutation-annot-after-certify-p2.pdf"

# --- verification ---------------------------------------------------------
expect_for() {
  case "$1" in
    01-visible-rsa) echo "Signature valid; identity unknown (self-signed). Visible field with appearance, bottom-left." ;;
    02-invisible-rsa) echo "Signature valid; identity unknown (self-signed). Invisible (listed in the signature panel only)." ;;
    03-ecdsa-p256) echo "Signature valid; identity unknown (self-signed). ECDSA P-256 / SHA-256." ;;
    04-rsa-pss) echo "Signature valid; identity unknown (self-signed). RSASSA-PSS (Acrobat 2020+ supports it; older versions may report an unsupported algorithm)." ;;
    05-pades-b-b) echo "Signature valid; identity unknown (self-signed). Subfilter ETSI.CAdES.detached (PAdES B-B), no signing-time attribute." ;;
    06-existing-field) echo "Signature valid; identity unknown (self-signed). The pre-existing field SigField1 is filled (no new field)." ;;
    07-certify-p2) echo "Certified document: certification valid; identity unknown (self-signed); changes allowed: form fill and signing." ;;
    08-two-sequential) echo "Two signatures; the first reports 'signed, document has not been modified since' (later revision only adds a signature, allowed); both valid, identity unknown." ;;
    09-xref-stream-source) echo "Signature valid; identity unknown. Source file used an xref stream." ;;
    10-resign-already-signed) echo "Two signatures (RSA then ECDSA); both valid, identity unknown." ;;
    11-sha512-rsa) echo "Signature valid; identity unknown. RSA with SHA-512." ;;
    20-mutation-annot-after-sign) echo "Signature crypto still valid but 'document has been modified since signed' (annotation added after a non-certifying signature; Acrobat may show a warning or 'valid, with changes' depending on version)." ;;
    21-mutation-flipped-byte) echo "Signature INVALID: 'document has been altered or corrupted since the signature was applied'." ;;
    22-mutation-annot-after-certify-p2) echo "Certification INVALID/violated: annotation changes are not permitted under DocMDP P=2." ;;
    *) echo "?" ;;
  esac
}

{
  echo "# Rivet Acrobat interop corpus"
  echo
  echo "Generated by tools/qa/make-acrobat-corpus.sh. TEST material only."
  echo
  echo "| file | expected Acrobat result | rivet-pdf-signature-inspect | pdfsig |"
  echo "|---|---|---|---|"
} >"$OUT/index.md"

fail=0
for f in "$S"/*.pdf; do
  b="$(basename "$f" .pdf)"
  "$INSPECT" --pdf "$f" >"$OUT/logs/$b.inspect.txt" 2>&1 || true
  if command -v pdfsig >/dev/null; then
    pdfsig "$f" >"$OUT/logs/$b.pdfsig.txt" 2>&1 || true
    ps="$(grep -E 'Signature Validation|Certificate Validation' "$OUT/logs/$b.pdfsig.txt" | sed 's/^ *- //' | sort -u | paste -sd';' -)"
    pdfsig_summary="$(grep -c 'Signature #' "$OUT/logs/$b.pdfsig.txt") sig; $ps; $(grep -m1 -E 'total document signed|Total document signed' "$OUT/logs/$b.pdfsig.txt" | sed 's/^ *- //' || true)"
  else
    pdfsig_summary="pdfsig not installed"
  fi
  states="$(grep -E '^  state:' "$OUT/logs/$b.inspect.txt" | sed 's/^  state: //' | paste -sd'; ' -)"
  echo "| signed/$b.pdf | $(expect_for "$b") | $states | $pdfsig_summary |" >>"$OUT/index.md"
  printf '%-40s inspect: %s\n' "$b" "$states"
done

cat >>"$OUT/index.md" <<EOF

## Source PDFs (src/)

- plain.pdf: one page, classic xref.
- empty-sig-field.pdf: one page with an empty signature field named SigField1.
- xref-stream.pdf: one page, cross-reference stream.
- already-signed.pdf: plain.pdf signed (invisible) by the RSA identity.

## Logs

logs/*.inspect.txt and logs/*.pdfsig.txt hold the full verifier output per file.
EOF

cat >"$OUT/README.md" <<EOF
# Rivet Acrobat corpus: how to use

THROWAWAY TEST MATERIAL. These identities are self-signed ("RIVET TEST ..."),
their password is public, and they must never be trusted for anything real.
Generated outside the Rivet repository; do not commit.

Identities (identities/*.p12), password for all: \`$PASS\`
- rsa2048.p12      RSA 2048
- ecdsa-p256.p12   ECDSA P-256
- rsa-pss.p12      RSA 2048 key, used for RSASSA-PSS signatures
(.crt.pem next to each is the certificate alone, for "Trust this certificate" tests.)

Import into Acrobat: Preferences > Signatures > Identities & Trusted Certificates >
Digital IDs > Add ID > "My existing digital ID" > PKCS#12 file. Then sign a PDF from
src/ in Acrobat to test Acrobat-signed-then-Rivet-verified, or open signed/*.pdf
to see what Acrobat says. To make the identity trusted instead of unknown: Trusted
Certificates > Import the .crt.pem, then Edit Trust > "Use this certificate as a
trusted root". Re-open the file: the "identity unknown" warning should disappear.

See index.md for each file and the result Acrobat should show.
EOF

echo "corpus: $OUT"
