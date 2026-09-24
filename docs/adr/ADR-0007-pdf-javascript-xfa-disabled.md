# ADR-0007: PDF JavaScript and XFA are disabled

Status: Accepted

Date: 2026-09-24

## Context

PDF files may embed JavaScript (PDF 1.7+ / ECMA-262 based, executed by
conformant readers for form logic and multimedia) and XFA (XML Forms
Architecture, a proprietary Adobe form model with its own scripting).
Executing either means running untrusted, document-controlled code inside
the application's process.

Rivet's posture treats PDFs as untrusted input (`docs/ARCHITECTURE.md`,
section 9): no document JavaScript execution, no automatic link launching,
no embedded content execution. PDFium implements both features, but its
JavaScript support requires building it with V8, and XFA support is enabled
separately - both are compile-time switches.

## Decision

1. **Rivet never executes document JavaScript or XFA scripts**, regardless of
   build configuration.
2. The recommended (and documented) PDFium build for Rivet sets
   `pdf_enable_v8 = false` and `pdf_enable_xfa = false`
   (`docs/BUILDING_PDFIUM.md`). V8 is not compiled into the linked PDFium,
   so there is no script engine in the process at all.
3. No Rivet code path invokes document-level actions: no automatic link
   launching, no embedded content (multimedia, file attachments) execution.
4. Static PDF form fields (AcroForm widgets) may be rendered and, later,
   filled; dynamic XFA forms are out of scope.

## Consequences

- Attack surface shrinks substantially: no JS/V8 engine is present in the
  process, eliminating whole vulnerability classes used against PDF readers.
- JavaScript-driven PDFs (auto-print tricks, form validations, embedded
  multimedia) degrade to static documents; their static content still
  renders. This is an accepted product trade-off in exchange for security.
- XFA-only forms (rare in practice, deprecated in the PDF 2.0 standard) will
  not be interactive. AcroForm forms remain supported.
- The decision is enforced in two places: the documented PDFium `args.gn`
  (V8 and XFA not built) and the absence of any script/action invocation in
  Rivet code. A developer linking a PDFium built with V8 enabled gains no
  execution path, because Rivet never calls the scripting APIs - but such a
  build is explicitly discouraged and not supported.
- If form-script compatibility is ever reconsidered, it must come with a new
  ADR covering sandboxing and an explicit security review.

## Rejected alternatives

- **Build PDFium with V8 enabled and execute JavaScript in a controlled
  sandbox**: still runs untrusted code in-process; PDF reader exploits
  demonstrate this is high-risk, and the sandboxing effort is out of
  proportion to the benefit for an editor.
- **Enable XFA for form fidelity**: proprietary, deprecated technology;
  expands attack surface for a marginal feature.
- **Enable features but gate them behind a user prompt**: per-file trust
  prompts train users to click through warnings and still run untrusted
  code; rejected.
- **Defer the decision** (build with JS on now, decide later): the build
  configuration is decided once in `docs/BUILDING_PDFIUM.md`; shipping V8 by
  default and removing it later is the wrong direction and harder to walk
  back.
