# ADR-0009: Background save, editing lock, rebase and dirty lifecycle

Status: Accepted

Date: 2026-09-29

## Context

A save reserializes the whole document (ADR-0008) and then atomically
replaces the destination (ADR-0010). That can take long enough to be
noticeable, so it cannot run on the main thread, yet:

- the document keeps being displayed and interacted with while it runs;
- the state that ends up on disk must be exactly the state that is then
  considered "saved" (dirty tracking must not lie);
- the tab may be closed, the window closed or the application quit while a
  save is in flight;
- after a successful save the session's sources should be the new file
  (the old file may have been replaced; deleted pages should stop pinning
  old documents).

## Decision

### Pipeline (`editor::DocumentSaver`, `app::FileController`)

```text
main:    job = makeSaveJob(session, dest)   snapshot + PdfAssemblyRequest
         session.setEditingLocked(true)
worker:  runDocumentWrite(engine, job)
           AtomicFileWriter::begin (temp file next to dest)
           engine.assembleDocument -> BufferedAtomicSink -> writer
           commit (fsync, rename)           ADR-0010
           [Save] DocumentSession::prepareRebase: reopen the written file
                  with the same credentials, read page metadata + labels
main:    (via IMainThreadDispatcher) handleSaveCompleted:
           success -> session.rebaseOnto(target, newPath?)
                      retitle tab if Save As, session.markSaved()
           always  -> session.setEditingLocked(false)
```

- The `DocumentWriteJob` owns the `PageModelSnapshot`; its entries keep the
  base and every imported document alive until the worker is done, and the
  request's raw document pointers point into it. The worker never reads
  session state.
- Output goes through a 256 KiB buffered sink into
  `AtomicFileWriter`; cancellation is polled between chunks and before the
  commit. Any failure before the rename aborts the writer, leaves the
  destination untouched and removes the temp file (ADR-0010).
- A failed reopen after a successful write does not fail the save: the
  session keeps its previous base (still valid, it reads its own open
  descriptor) and a status note is shown.

### Editing lock

While a save is in flight `DocumentSession::setEditingLocked(true)` makes
`execute()` (reports `Unsupported`), `undo()` and `redo()` refuse, and the
page-editing controller reports "A save is in progress". The page model
therefore cannot change under the save: the snapshot that is written is the
state that is marked saved and rebased. Viewing, text selection, search,
print and extract keep working. The lock is released on success **and** on
failure (a failed save leaves the document dirty and editable).

### Rebase onto the saved file

`DocumentSession::rebaseOnto` (main thread) switches the session onto the
file just written from the current model (page i of the file = model entry
i): every entry keeps its `PageId` and `contentRevision`, its source becomes
the new document at `sourcePageIndex = position` and its view/boxes the new
file's native ones (display-identical, so tiles and text stay valid; an
entry that differs beyond float noise gets a fresh revision). References to
the previous base and imported documents are dropped, base info, page labels
and path are replaced, cached links and outline are reset, and the **undo
history is cleared** (recorded commands hold entries of the previous
documents, and a deleted page does not exist in the new file). The
command-stack `stateId` is kept, so `markSaved()` stays exact. One page-model
change is published (`orderChanged = false`).

### Operation identity and async lifetime

- `TabId`s are never reused; `FileController` mints a fresh generation for
  every save/import operation. A completion applies only while its tab still
  exists and is Ready; imports additionally only while their generation is
  registered in `liveImports_`. Completions posted to the main thread are
  guarded by a heap-owned `alive_` flag so a destroyed controller is never
  touched.
- A save whose tab was closed meanwhile drops its result: the file was still
  written atomically, there is no session left to rebase.
- One save at a time per shell (`activeSave_`): an interactive Save/Save As
  during a save is reported and not queued; saves started by the quit
  lifecycle are queued and chained. Extract and import may overlap a save
  (extract never touches the model; import inserts through a command, which
  the editing lock refuses while a save runs).
- **Dirty is cleared only on success**: `markSaved()` is called exclusively
  in the success path of `handleSaveCompleted`.

### Dirty close / quit (`platform::IAppLifecycle`, `IAlertService`)

- Tab close: a clean tab closes. A dirty tab asks Save / Don't Save /
  Cancel. Don't Save discards and closes; Save starts a save and closes the
  tab when that save settles (`closeAfterSave_`); a tab already saving just
  gets the close intent. If no alert service is available the answer is
  Cancel: data is never discarded silently.
- Window close: one dirty tab behaves like a tab close; several use the
  platform review prompt (Review / Discard All / Cancel), Review asking per
  tab. "Save" performs the saves and leaves the window open (there is no
  programmatic window-close to re-trigger).
- Quit: prompts the same way, then defers the platform quit reply until every
  accepted save has settled (`pendingQuit_.outstanding`), so termination never
  races an in-flight save.

### Import, merge, extract

- **Import before/after the current page, Merge (append)**: open panel;
  worker opens the source with the engine and reads its page metadata
  (`PageModel::describeAllPages`); on the main thread an `InsertPagesCommand`
  inserts all pages as one undoable block with new ids and native views.
  Password-protected sources are rejected ("encrypted imports are not
  supported"). Insertion is atomic on the model.
- **Extract**: save panel, then a worker assembles a `Fresh` document from
  the selected pages (model order) into the chosen file. The source
  document/model is never modified, and dirty state is unaffected.

## Consequences

- The UI stays responsive while saving; the cost is that editing pauses for
  the duration of a save (and PDFium rendering stalls during assembly, which
  holds the call gate - ADR-0006).
- Undo history does not survive a save. This is a deliberate trade-off; the
  alternative (re-targeting commands onto the new file) is not generally
  possible for deletes.
- Correctness of the dirty flag rests on three facts: the model cannot change
  during a save, `stateId`s are never reused, and `markSaved()` is success
  only.
- Known gap: `saveSettled` runs the deferred close intent and the quit
  bookkeeping for failed saves too, so a failed save that was started from
  a "Save" answer to a close/quit prompt can still close the tab / complete
  the quit. The status bar reports the failure, but the unsaved changes are
  lost with the tab. A follow-up should keep the tab/quit pending on failure
  (or re-prompt).
