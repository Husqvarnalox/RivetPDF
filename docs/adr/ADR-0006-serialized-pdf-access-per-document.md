# ADR-0006: Serialized PDF access per document via SerialExecutor

Status: Accepted

Date: 2026-09-24

## Context

PDFium's document APIs are not thread-safe: a given `FPDF_DOCUMENT` and its
pages must be accessed by one thread at a time. Rivet nevertheless needs
concurrency: rendering must happen off the main thread (ADR-0005), multiple
documents will be open simultaneously (planned tabs), and tile requests
arrive from the UI while earlier requests are still in flight.

Candidate models:

1. One dedicated OS thread per open document.
2. A mutex around the document handle, with any worker thread locking it for
   each PDFium call.
3. Reworking or wrapping the engine so every call is internally thread-safe.
4. Serializing all work for a document onto a shared pool.

## Decision

Each open document gets a **`SerialExecutor`** (one per `DocumentSession`)
over the **shared `TaskScheduler`** (`std::jthread` worker pool):

- Exactly one task of a given `SerialExecutor` runs at any moment, in FIFO
  post order. There is **no dedicated OS thread per document**; idle
  executors cost nothing.
- All PDFium access for a document (page info queries, tile rasterization
  via `PdfDocument::renderPage`) is dispatched through that document's
  executor, so the engine's single-thread-per-document requirement is
  satisfied by construction rather than by locks at call sites.
- `DocumentRenderer` (editor layer) posts rasterization jobs to the
  session's executor, dedupes requests before posting, and stores results in
  the `TileCache`.
- Render results are marshaled back to the main thread via
  `IMainThreadDispatcher` (implemented by the platform layer); render
  callbacks fire on the main thread in production. All widget/event handling
  is main-thread-only.
- The shared `TaskScheduler` discards not-yet-started tasks at shutdown and
  lets in-flight tasks finish; `SerialExecutor::cancelPending()` drops queued
  tasks when a session closes.

## Consequences

- No per-document thread ceiling: the worker pool is fixed
  (`clamp(hardware_concurrency - 1, 2, 8)` by default), so memory and
  scheduler load stay bounded as tabs multiply.
- One rasterization at a time per document; the pool stays busy across
  documents because executors are multiplexed over the shared workers.
- FIFO ordering makes request handling predictable (visible tiles requested
  first are rendered first), which pairs with `RenderPriority` hints for
  scheduling intent.
- No fine-grained locking around the document handle in adapter code; the
  executor is the synchronization point. Code must not touch a
  `PdfDocument` outside its executor.
- Shutdown semantics are explicit: pending tasks are discarded, sessions
  cancel their queues, and results for cancelled work either report
  `Cancelled` or do not fire.
- Destroying a `SerialExecutor` from inside one of its own tasks is
  unsupported (documented lifetime rule); sessions are destroyed from the
  main thread.

## Rejected alternatives

- **One OS thread per document**: does not scale - each open document
  (planned tabs) reserves a stack and a scheduling slot whether busy or
  idle; dozens of tabs would hold dozens of threads.
- **Mutex-guarded shared document access**: correct but ad hoc; every
  adapter call site must remember the lock, deadlocks appear when PDFium
  re-enters, and FIFO fairness (visible-first rendering) would need extra
  machinery anyway.
- **Making the engine layer internally thread-safe**: PDFium is a third-party
  engine; wrapping every call in internal locks duplicates what the
  executor already provides and invites subtle races.
- **Main-thread rendering**: blocks the UI on rasterization; rejected by the
  performance goals.
- **Unbounded task pool (thread-per-task)**: scheduler thrash and unbounded
  concurrency against a single document handle; a fixed pool plus per-document
  serialization is simpler and safer.
