# ADR-0010: Atomic save by temp file + rename

Status: Proposed

Date: 2026-09-26

## Context

Rivet is gaining Save / Save As for edited documents (page reorder, rotate,
crop, insert, delete). A save rewrites the whole PDF: PDFium serializes the
assembled document through a byte sink. Writing that stream directly over
the user's file is unsafe - a crash, power loss, full disk, cancellation or
a serializer error half way through would leave a truncated, unreadable
document, and the source document Rivet is still reading from would be
corrupted under its own feet.

Requirements:

- The destination is either the complete old file or the complete new file,
  never a mix, including after a crash.
- Every failure (I/O error, disk full, permission, cancellation, serializer
  error) leaves the destination untouched and no stray temp file.
- Saving over the document's own source file must work.
- Errors are specific enough for a useful message ("disk full",
  "read-only volume", "permission denied") - not just "I/O error".
- `core` stays portable; the Windows path must be designed even if it is
  not implemented yet.

## Decision

`core::io::AtomicFileWriter` (src/core/io) implements the classic
write-temp-then-rename protocol:

1. Resolve the destination: follow symlinks (up to 32 hops), refuse a
   directory, a non-regular file, a missing parent directory, an over-long
   name, a destination that is not writable by us, or an existing file when
   the caller did not allow overwriting.
2. Create a uniquely named hidden temp file (`.<name>.rivet-<random>.tmp`)
   **in the destination directory** with `O_CREAT | O_EXCL`, so the final
   rename stays on one volume and is atomic. The file is narrowed to mode
   0600 before the first byte is written.
3. Stream bytes through `core::io::IByteSink` (same shape as
   `pdf::IPdfByteSink`; the editor adapts one to the other). Partial writes
   and `EINTR` are handled; a cancellation flag is checked before each write.
4. Commit: copy extended attributes (Apple, best effort), apply the final
   mode (existing file's mode, or `0644 & ~umask` for new files - the umask
   is observed race-free from the temp file's creation mode instead of
   calling the process-global `umask(2)`), restore owner/group best effort,
   `F_FULLFSYNC` (Apple; `fsync` elsewhere or as fallback), verify the file
   size equals the bytes written, close, `rename(2)` over the destination
   (`renamex_np(RENAME_EXCL)` on Apple when overwriting is not allowed),
   then fsync the directory so the new entry is durable.
5. Any failure before the rename closes and unlinks the temp file; the
   writer becomes failed (sticky error). The destructor aborts an
   uncommitted writer.

errno values map to `core::ErrorCode` with user-presentable messages
(no paths, for privacy): `EACCES/EPERM/EROFS` -> PermissionDenied,
`ENOSPC/EDQUOT` -> DiskFull, `ENOENT/ENOTDIR` -> NotFound,
`EISDIR/ENAMETOOLONG/ELOOP` -> InvalidArgument, `EEXIST` -> AlreadyExists,
cancellation -> Cancelled, anything else -> Io. PermissionDenied, DiskFull
and AlreadyExists are new codes added for this.

A test-only fault injector (`AtomicWriteOptions::faultInjector`) simulates
failures at the write, sync, rename and directory-sync points, because disk
full and fsync errors cannot be provoked portably.

Decisions on edge cases:

- **Symlinks**: write through to the final target and keep the link. Saving
  must not silently turn a link into a regular file.
- **Read-only destination** (mode without write permission, or locked): the
  save is refused with PermissionDenied, although POSIX would allow renaming
  over it. A read-only file is a user decision; the app offers Save As.
- **Same path as the source**: supported. The source document is read
  through a descriptor opened before the save; `rename(2)` only swaps the
  directory entry and the old inode lives until that descriptor closes.
- **Directory fsync failure after the rename**: cannot be rolled back (the
  new content is already visible); it is logged and the save reports
  success.
- **Extended attributes**: preserved on Apple via `fcopyfile(COPYFILE_XATTR)`
  (Finder tags, quarantine, etc.), best effort. Not preserved on other
  platforms, nor are ACLs, file creation dates, or hard links (other links
  keep pointing at the old inode). Documented limitation.
- **No-overwrite race** outside Apple: checked right before `rename(2)`
  (small TOCTOU window; `renameat2(RENAME_NOREPLACE)` is a possible
  follow-up on Linux).

## Consequences

- Saves are crash-safe and failure-atomic on POSIX volumes that implement
  `rename(2)` atomically (APFS, HFS+, ext4, XFS, ...). Network and FAT-style
  volumes give weaker guarantees; the protocol is still the best available.
- The destination gets a new inode on every save. File identity-based
  tooling (hard links, some backup tools) sees a new file; Finder aliases
  and bookmarks resolve by path on macOS and keep working.
- A save needs free space for a full second copy of the document.
- `F_FULLFSYNC` makes a save on Apple take tens of milliseconds longer; the
  writer runs on a background worker, never on the main thread.
- **Windows** is not implemented: `begin()` returns
  `ErrorCode::Unsupported`. Planned design: create the temp with
  `CreateFileW(CREATE_NEW)` in the destination directory, `WriteFile` +
  `FlushFileBuffers`, then `ReplaceFileW` when the destination exists (keeps
  its ACLs, attributes and alternate streams) or
  `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` for new
  files. Limitation: Windows cannot replace a file that is open without
  `FILE_SHARE_DELETE`, so the document source must be opened with that
  share mode (or the replace fails with a sharing violation that is
  reported as PermissionDenied).
