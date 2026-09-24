# ADR-0005: Tile-based rendering with a byte-bounded LRU cache

Status: Accepted

Date: 2026-09-24

## Context

Rivet must render documents with hundreds or thousands of pages at arbitrary
zoom on displays of any density, with bounded memory and smooth scrolling
(README performance goals: lazy page loading, tile-based rendering, bounded
memory, background rendering, cache eviction).

The naive strategies fail:

- Rasterizing whole pages does not scale: at high zoom a single page
  (e.g. A4 at 64x with 2x backing scale) produces bitmaps far beyond memory
  budgets, and repainting a full page on every scroll wastes work for the
  small visible fraction.
- Painting vector content directly for every frame couples the UI frame rate
  to PDF rasterization speed - rasterization happens on worker threads and
  cannot be in the paint path.
- Unbounded caches leak memory in long editing sessions; page-sized caches
  waste most of their bytes on off-screen content.

## Decision

Rendering is **tile-oriented from day one**:

1. **Tile grid**: pages are rasterized in 512 x 512 device-pixel tiles,
   addressed by `(tileX, tileY)` on the page's tile grid.
2. **Cache identity**: `TileKey = (DocumentId, PageId, RenderScaleKey,
   tileX, tileY)`. `RenderScaleKey` quantizes zoom **UP** to multiples of
   1/64 (`ceil(zoom * 64) / 64`, clamped to `[0.10, 64.0]`), so the raster is
   never produced below the requested resolution; the painter scales down by
   less than 1/64. Zoom levels that quantize to the same key share tiles,
   which absorbs continuous pinch-zoom into a small set of renders.
3. **Render contract**: `IRenderSource::requestRender(RenderRequest,
   RenderPriority, onDone)` is the only render entry for views.
   `RenderRequest` pairs the `TileKey` with `RasterParams` (page display
   rect, `devicePixelsPerPoint`). Priorities: `Visible`, `Impending` (scroll
   lookahead), `Prefetch`.
4. **Cache**: `TileCache` is an LRU bounded by an exact byte budget
   (default 256 MiB, hard bound - the total of exact `Bitmap::sizeBytes()`
   never exceeds the budget; a bitmap larger than the whole budget is
   refused). Entries are stamped with the **document revision** they were
   rendered for; a `get()` with a different revision is a miss that lazily
   drops the stale entry, so edits invalidate tiles without a full sweep.
   The cache is mutex-guarded and usable from scheduler and main threads.
5. **Lazy page loading**: only tiles for visible and impending pages are
   scheduled; pages are loaded on demand, so large documents never render
   fully into memory.

## Consequences

- Memory is bounded regardless of document size or zoom; the budget is a
  hard invariant, not a soft target.
- Scrolling and zooming stay smooth: the paint path only probes the cache
  (`cachedTile`) synchronously; rasterization is asynchronous and
  off-thread.
- Continuous zoom produces at most a bounded number of distinct scale keys;
  tiles render once per quantized scale and are reused.
- Sub-1/64 zoom deltas show a sub-pixel rescale instead of triggering new
  renders - an accepted, visually negligible trade for cache stability.
- A document edit (future page editing) bumps the revision and invalidates
  tiles lazily; no expensive synchronous invalidation pass.
- Keys include `DocumentId`, so multiple documents (planned tabs) share one
  cache safely.
- Slight oversampling (raster at ceil(zoom*64)/64) trades a little extra
  memory and raster work for the guarantee that downscaled output is always
  sharp.

## Rejected alternatives

- **Whole-page rasters**: unbounded bitmap size at high zoom; poor cache
  granularity; unusable for large-format pages.
- **Direct vector painting each frame**: puts PDF rasterization in the paint
  path; frame rate would depend on page complexity. (The platform paint
  context does composite cached tile bitmaps per frame.)
- **Round-to-nearest or round-down scale quantization**: round-down renders
  below requested zoom (blurry, and violates the never-rasterize-below-zoom
  requirement); round-to-nearest causes visible scale flips while pinching.
  Round-up keeps output sharp and flips amortized.
- **Unbounded or entry-count-bounded cache**: byte budgets match actual
  memory cost (tile bytes vary with backing scale); entry counts do not.
- **Revision-sweep invalidation** (clear affected keys on edit): O(cache)
  work on every edit; lazy stale-on-read is cheaper and sufficient.
- **Eager full-document loading**: contradicts lazy page loading; documents
  with thousands of pages would saturate memory and startup time.
