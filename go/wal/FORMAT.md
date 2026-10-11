# `.nxsw` v2 — durable Nyxis write-ahead log

Package `github.com/nyxis-io/nyxis-drivers/go/wal` implements an append-only,
crash-safe log of NYXO objects with a caller-defined schema. Version 1 of
`.nxsw` (`0x0100`, span-only, unframed, written by the Rust `SpanWal`) is not
read by this package.

## Directory layout

```
<dir>/LOCK                   exclusive flock held by the open WAL
<dir>/<segment:016x>.nxsw    segments, numbered from 1, contiguous
<dir>/<segment:016x>.nxsw.tmp  segment being created (removed on Open)
```

Exactly one segment is live: the highest-numbered one. Every lower-numbered
segment is sealed and immutable.

## Segment header

All integers are little-endian.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `NXSW` (`0x5753584E`) |
| 4 | 2 | version `0x0200` |
| 6 | 2 | flags |
| 8 | 4 | `schema_len` |
| 12 | `schema_len` | schema bytes |
| 12 + `schema_len` | 0–7 | zero padding so frames start 8-byte aligned |

Flags:

| Bit | Name | Meaning |
|---|---|---|
| `0x0001` | `SCHEMA_EMBEDDED` | always set |
| `0x0002` | `CRC_FRAMED` | always set in v2 |
| `0x0004` | `SEALED` | set by rotation; the file never changes again |

Schema bytes use the `.nxb` schema header encoding: `KeyCount u16`, one sigil
byte per key, then each key as NUL-terminated UTF-8, zero-padded to a multiple
of 8. Supported sigils are the ones the Go writer emits: `=` i64, `~` f64,
`?` bool, `"` string, `<` binary, `@` time (Unix ns), `^` null.

## Frames

```
frame := [payload_len u32][crc32c(payload) u32][payload]
```

- `payload` is one NYXO object as produced by `nxs.ObjectEncoder`; objects are
  always a multiple of 8 bytes, so frames stay 8-byte aligned.
- `crc32c` is Castagnoli (`hash/crc32.MakeTable(crc32.Castagnoli)`).
- A frame is valid when `payload_len >= 8`, `payload_len` is a multiple of 8,
  the whole payload is present, and the CRC matches. A CRC-valid payload that
  does not decode as an object of the segment schema is corruption.
- `Append` rejects records larger than `MaxRecordBytes` (`ErrRecordTooLarge`).
  Recovery does not use that option, so reopening with a smaller limit can
  never truncate frames that were acknowledged under a larger one.
- An `Offset` is `(segment, pos)` where `pos` is the byte offset of the frame
  header in the segment file. The zero `Offset` precedes every record.

## Schema per segment

Each segment embeds the schema its frames were written with. A new schema may
only append fields: same keys and sigils, in order, plus zero or more trailing
fields (`Schema.IsAdditiveOf`). Opening with an additive schema seals the live
segment and starts a new one; any other difference returns `ErrSchemaMismatch`
before any file is modified. Readers decode each frame with its own segment's
schema, so fields added later read as absent in older records.

**Upgrades are roll-forward only.** After a schema extension, an older binary
opening the directory gets `ErrSchemaMismatch`.

## Durability

`durableEnd` is the end of the last frame covered by a successful sync of the
live segment. Only frames before `durableEnd` are acknowledged to appenders or
visible to live scanners.

| `SyncPolicy` | `Append` returns |
|---|---|
| `SyncGroup` (default) | after a sync that covers its frame; one flusher writes and syncs everything queued while the previous sync ran |
| `SyncEachAppend` | after a sync dedicated to its frame |
| `SyncManual` | after the frame is written; it becomes durable and visible on the next `Sync` |

Sync primitives:

- Linux: `fdatasync` for frames; `fsync` for header changes and directories.
- darwin: `os.File.Sync`, which issues `fcntl(F_FULLFSYNC)` so the drive cache
  is flushed; plain `fsync` there does not reach stable storage.

## Failure semantics (sticky)

Any short write or sync error fails every waiter in the affected batch and
moves the WAL to a failed state: later `Append`, `Sync`, and `Rotate` return
`ErrWALFailed`. The only way out is `Close` then `Open`.

In the failed state `Close` truncates the live segment to `durableEnd` and
syncs it. Frames of the failed batch are removed even when they were fully
written and pass their CRC from the page cache: after a failed `fsync`, Linux
may mark dirty pages clean without writing them, so those frames could read
back as garbage after power loss, and recovery would then also truncate every
frame acknowledged after them. If the truncate or sync fails, `Close` returns
the error and the WAL stays failed; callers retry `Close` before `Open`.

**Residual window.** A sync error followed by a process crash before `Close`
leaves the failed batch's frames in place. `Open` keeps them if they are valid
in the page cache, and a later power loss before they reach disk can truncate
frames acknowledged after the restart. Callers should alert on any append
error; the window is accepted.

## Recovery on `Open`

`Open` first takes an exclusive `flock` on `<dir>/LOCK` (`ErrLocked` if another
open WAL, in this or another process, holds it). It then reads every segment
header and checks the schema against the highest segment before modifying
anything. Then:

1. Leftover `*.nxsw.tmp` files are removed.
2. A non-highest segment without `SEALED` (impossible under the rotation order,
   tolerated) is validated end to end and sealed if every frame is valid
   (`SealedOnOpen`); otherwise `Open` fails with `ErrCorruptSegment`.
3. If the highest segment is sealed (crash during rotation), the next live
   segment is created (`CreatedLive`).
4. The live segment is scanned from its header. The first invalid frame and
   everything after it is a torn tail: the file is truncated to the end of the
   last valid frame and synced (`TruncatedBytes`). Those frames were never
   acknowledged.
5. An additive schema change rotates (`SchemaRotated`).

Frames in sealed segments are never truncated. An invalid frame there is
corruption: scanners return `ErrCorruptSegment` and leave the file untouched.

## Rotation order

Each step is crash-safe:

1. Sync the live segment.
2. Set `SEALED` in its header and sync.
3. Write the next segment's header to `<n>.nxsw.tmp`, sync, rename to
   `<n>.nxsw`.
4. Sync the directory.

The flusher rotates after a batch once the live segment reaches
`SegmentBytes`; `Rotate` forces it.

## Scanning

`OpenScanner(dir, after)` reads segments in order, starting strictly after
`after`; at the end of the highest segment an invalid or partial frame ends the
scan without error. `(*WAL).NewLiveScanner(after)` reads up to the WAL's durable
end as of each `Next` call; `Next` returns false there and true again once more
records are durable.
