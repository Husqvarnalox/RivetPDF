#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Builds the fixtures of the scripted QA scenarios into $QA_FIX (default
# build/qa-fixtures): generated PDFs (tools/qa/make_qa_fixtures.cpp), the
# repository's password/outline fixtures, a long text PDF and the Markdown
# inputs. The "scan" PNG for OCR needs macOS (sips); elsewhere it is skipped.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="${QA_FIX:-$root/build/qa-fixtures}"
mkdir -p "$out"

"${CXX:-c++}" -std=c++2b -O1 -I "$root/tests/pdf" "$here/make_qa_fixtures.cpp" -o "$out/make_qa_fixtures"
"$out/make_qa_fixtures" "$out"
cp "$root/tests/pdf/fixtures/password.pdf" "$root/tests/pdf/fixtures/outline.pdf" "$out/"

# A long text PDF: cupsfilter on macOS; elsewhere the Markdown export of the
# architecture document is used instead (made by scenario 9 on demand).
if command -v cupsfilter >/dev/null 2>&1; then
    cupsfilter "$root/docs/ARCHITECTURE.md" >"$out/long.pdf" 2>/dev/null
fi

# The OCR input: page 1 of scan-source.pdf rasterized; scenario 8 exports
# scan.md (just that image) to an image-only PDF with Rivet itself.
if command -v sips >/dev/null 2>&1; then
    sips -s format png -Z 1700 "$out/scan-source.pdf" --out "$out/scan.png" >/dev/null
fi
printf '![scan](scan.png)\n' >"$out/scan.md"

cat >"$out/graphics.md" <<'MD'
# Graphics sample

A paragraph with **bold**, *italic* and `code`, and inline math $E = mc^2$.

- first item
- second item

$$
\int_0^1 x^2 \, dx = \frac{1}{3}
$$

```mermaid
graph TD
  A[Open] --> B{Edit?}
  B -->|yes| C[Save]
  B -->|no| D[Close]
```

| Column | Value |
|--------|-------|
| alpha  | 1     |
| beta   | 2     |
MD

cat >"$out/edit.md" <<'MD'
# Editing sample

First paragraph to edit.

- apple
- banana
MD
echo "fixtures in $out"
