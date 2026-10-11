---
room: wal
subdomain: go
source_paths: go/object.go, go/wal/
see_also: ["go/reader.md"]
hot_paths: wal/wal.go, wal/scanner.go, wal/frame.go
architectural_health: normal
security_tier: sensitive
---

# go/ — Standalone Objects & Write-Ahead Log

Subdomain: go/
Source paths: go/object.go, go/wal/

Durability-critical: any change to framing, sync order, or recovery must keep `wal_test.go` and `crash_test.go` green, including under `-race` and on Linux.

## TASK → LOAD

| Task | Load |
|------|------|
| Encode or decode one NYXO object without a file | wal.md |
| Append records durably to a journal | wal.md |
| Recover or scan a `.nxsw` v2 directory | wal.md |
| Change WAL sync policy, rotation, or failure semantics | wal.md |
| Inject I/O faults in WAL tests | wal.md |

---

# object.go

DOES: Encodes a single NYXO object for a fixed schema with per-slot sigils, reusing `Writer` so the bytes equal a data-sector object. Validates every write (sigil, slot range, duplicates) and reports the first error from `Bytes`. Decodes standalone objects after bounds-checking the header, bitmask, offset table, and each field's extent.
SYMBOLS:
- NewObjectEncoder(keys []string, sigils []byte) *ObjectEncoder
- (*ObjectEncoder) Reset / WriteI64 / WriteF64 / WriteBool / WriteStr / WriteBytes / WriteTime / WriteNull
- (*ObjectEncoder) Bytes() ([]byte, error)
- NewObjectDecoder(keys []string, sigils []byte) (*ObjectDecoder, error)
- (*ObjectDecoder) Decode(obj []byte) (*Object, error)
- DecodeObject(keys []string, sigils []byte, obj []byte) (*Object, error)
- (*Object) GetBytes / GetTime / Has
- ErrObjectTooLarge
DEPENDS: writer.go (Writer internals), nxs.go (Object, Reader)
PATTERNS: shared encoder path, validate-then-view
USE WHEN: Producing or reading log/journal records; not for `.nxb` files (use writer.go / nxs.go).

---

# wal/wal.go

DOES: Opens a WAL directory under an exclusive lock, recovers it (torn-tail truncation, interrupted rotation, schema rotation), and runs one flusher goroutine that batches appends, writes them, syncs, and only then acknowledges. Any write or sync error poisons the WAL; `Close` in that state truncates to the durable end.
SYMBOLS:
- Open(dir string, schema Schema, opts Options) (*WAL, RecoveryReport, error)
- (*WAL) Append(ctx, fill func(*nxs.ObjectEncoder)) (Offset, error)
- (*WAL) Sync / Rotate / Last / Close
- Types: Schema, Field, Options, SyncPolicy, Offset, RecoveryReport
- Errors: ErrRecordTooLarge, ErrSchemaMismatch, ErrCorruptSegment, ErrWALFailed, ErrLocked, ErrClosed
DEPENDS: object.go, wal/segment.go, wal/frame.go, wal/lock.go, wal/file.go
PATTERNS: single-writer group commit, sticky failure, durable-end publication
USE WHEN: Changing append, sync, rotation, or recovery behavior.

---

# wal/segment.go, wal/frame.go

DOES: Segment header and schema codec, segment naming and listing, crash-safe segment creation (`.tmp` + sync + rename + dir sync). CRC32C frame encoding and a buffered frame iterator that distinguishes a clean end from a torn or corrupt frame.
SYMBOLS:
- encodeHeader / readHeader / writeFlags / encodeSchema / decodeSchema
- listSegments / createSegment / segmentName / parseSegmentName
- appendFrame / newFrameIter / (*frameIter) next / setLimit / scanFrames
DEPENDS: hash/crc32, bufio
PATTERNS: length + CRC framing, schema per segment
USE WHEN: Changing the on-disk format (update FORMAT.md in the same change).

---

# wal/scanner.go

DOES: Reads frames in order across segments and schema boundaries, strictly after an offset. Offline scanners treat a bad frame at the end of the highest segment as the end; live scanners stop at the WAL's durable end and resume as it grows or rotates.
SYMBOLS:
- OpenScanner(dir string, after Offset) (*Scanner, error)
- (*WAL) NewLiveScanner(after Offset) (*Scanner, error)
- (*Scanner) Next / Record / Schema / Offset / Err / Close
DEPENDS: wal/frame.go, wal/segment.go, object.go
PATTERNS: bounded live tailing
USE WHEN: Replaying or tailing a journal.

---

# wal/file.go, wal/lock.go, wal/sync_*.go

DOES: The filesystem seam (`fileSystem`, `segmentFile`) with the OS implementation, `flock` on `<dir>/LOCK`, and platform sync (Linux `fdatasync`; darwin `F_FULLFSYNC` through `os.File.Sync`).
SYMBOLS:
- fileSystem, segmentFile, osFS, osFile
- lockDir / unlockDir
- dataSync / fullSync
USE WHEN: Porting to a new platform or adding fault injection points.

---

# wal/*_test.go

DOES: Behavior suite (header, lock, schema evolution, failure injection, recovery, rotation order, live scanning, fsync-before-ack), a SIGKILL crash harness that re-executes the test binary (`-wal.crash.iters`), a 32-writer concurrency test, and throughput/latency benchmarks. `fault_test.go` wraps the seam to inject short writes and sync/truncate/rename errors and to record operation order.
SYMBOLS:
- TestCrash_NoAckedRecordLost / TestCrashHelper
- TestGroupCommit_ConcurrentAppenders
- BenchmarkAppend_SyncGroup_32Writers / BenchmarkAppend_SyncEachAppend / BenchmarkScan_100k
- faultFS / faultFile / failNext
USE WHEN: Verifying durability after any WAL change.
