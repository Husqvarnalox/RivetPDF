# ADR-0048: Digital-signature workflow and Set Password UI

Status: accepted. Builds on ADR-0044/0045 (signing, LTV), ADR-0046 (encryption), ADR-0047 (remote signing).

## Context

The signer, verifier, identity stores, time-stamp client and encryptor exist as
portable libraries. Phase 8 adds the retained-UI workflow around them: choose an
identity and PAdES level, sign with a PIN prompt, validate signatures, and set a
password. The UI must never claim more than the evidence supports, must keep
secrets out of logs, and must never damage the user's file.

## Decisions

1. Save-As copy only, for both Sign and Set Password. PDFium cannot write an
   encrypted file and re-saving a signed file invalidates earlier signatures, so
   both features write a new file chosen in the save panel. The open tab keeps
   pointing at the original, which is never modified. A destination equal to the
   source is refused. Output is written atomically (temp file, rename) with the
   cancel flag honoured; cancelling leaves no file.
2. Controllers are portable (`src/app`), views are retained widgets
   (`SecurityDialogs`), and the shell is reached through the `IDocumentSecurityHost`
   seam. All controllers use `BackgroundRunner` (a `core::AsyncScope` plus the
   main-thread dispatcher): signing, encryption and verification run off the
   main thread, completions are delivered on the main thread and never under a
   lock, and destruction drains workers first.
3. Source of the bytes. An unsigned document is captured through the existing
   save pipeline into a private temp file and read back. An already-signed
   document is signed from its saved file bytes (never re-saved); if it is dirty
   or unsaved the command is refused with an explanation. Encrypted documents
   cannot be signed; signed or encrypted documents cannot get a password.
4. Levels and the network. B-B is offline. B-T, B-LT and B-LTA need a TSA URL
   (https only, validated; the field is shown only for those levels), network
   availability, the online-validation gate not being Off, and configured
   time-stamp anchors (plus signer anchors for LT/LTA). Failures of these
   pre-checks are reported before the save panel opens. Consent Ask/On is
   enforced by the gate itself at request time; the prompt is forwarded to the
   view on the main thread (`ConsentPromptAdapter`). The default mode is Ask and
   is not persisted in this phase. Signing never silently downgrades the level
   when a time-stamp cannot be obtained; if the signer reports a lower level,
   the completion message says so.
5. Trust anchors come from `<appdata>/trust/` (`signer-anchors.pem`,
   `timestamp-anchors.pem`, `accredited-cas.txt`). The OS trust store is never
   used. Missing files mean "no anchors": signatures then show as valid but
   untrusted, and time-stamped levels are refused.
6. The validation panel verifies the saved file offline only (no revocation
   fetching) and says so in its footnote together with the legal disclaimer.
   Each row shows verifier status, signer certificate summary, time-stamp info,
   the PAdES level reached and an evidence-based classification.
7. Qualification wording is evidence only. `qualificationLabel` maps a class to
   text; the qualified-indicators wording is reachable only for a provider
   declared as a certified CSP. Software keys, tokens, platform keystores and
   remote services are capped at "advanced indicators; not shown to be
   qualified". A file alone does not reveal key custody, so the panel classifies
   as software and states that custody cannot be checked. Rivet never states
   that a signature is a QES.
8. Secrets. PINs and passwords are `SecretBuffer`s, moved into the job, cleared
   as soon as the encryptor/identity no longer needs them (success, failure and
   cancel). The `std::string` copies required by the `encryptPdf` API are zeroed
   immediately after the call. Nothing secret is logged or placed in a status
   message.
9. Errors are mapped to user-readable text in `SecurityText` (sign, crypto and
   encrypt error codes); raw codes are not shown as the primary message.

## Known limits

- `TextField` stores its text in a `std::string`. The dialogs copy it into a
  `SecretBuffer` and overwrite the field with an empty string, but earlier
  reallocations of the field's string may leave copies in freed heap memory.
  A secure text-entry widget is future work.
- The PIN prompt blocks the signing worker (polling every 50 ms) while the
  user answers; cancel and shutdown wake it.
- The dialogs, panels and menu items were built and unit-tested through their
  controllers with fakes; they were not launched or checked visually in this
  phase. Layout, focus order and the look of the panels still need a manual pass.
- Certification (DocMDP) signatures, signing into an existing empty field and
  removing a password are not part of this UI.

## Tests

`tests/app`: `rivet_app_security_tests` covers text/label mapping (including no
false qualified claim), Set Password and Digital Signature controllers with fake
host/store/signer (consent Off/Ask/On, PIN wrong/cancel, cancel, signed/encrypted
refusal, secrets cleared), the validation controller with a fake verifier, and an
integration test with the real signer (runtime-generated key) and encryptor.
