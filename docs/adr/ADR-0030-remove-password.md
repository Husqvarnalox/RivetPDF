# ADR-0030: Remove password on Save As

Status: Accepted

Date: 2026-10-16

## Decision

"Remove Password and Save As..." is enabled for a PDF tab whose document was
opened with a password (`PdfDocumentInfo::isEncrypted`). It runs the normal
Save As pipeline (atomic write, rebase onto the new file) with
`PdfAssemblyRequest::removeSecurity`, which makes the PDFium backend save
with `FPDF_REMOVE_SECURITY`. The saved file carries no encryption; the tab
follows it like after any Save As, so no password is needed afterwards. The
password the user typed to open the document is the authority to remove it;
nothing is stored. Choosing the document's own path is allowed (it replaces
the file with the unprotected copy after the usual save panel confirmation).

`removeSecurity` is refused together with `removeMetadata` (they are
different save modes in PDFium). Other saves keep the document's encryption
as before.

## Deferred

Adding a password needs an encrypting writer, which PDFium does not provide.
It stays deferred until a crypto dependency is approved.

## Consequences

- No new dependency; the only new surface is one request flag and one menu
  command.
- Test: `password.pdf` (user password "rivet") saved with removal reopens
  without a password; without the flag the saved copy stays encrypted.
