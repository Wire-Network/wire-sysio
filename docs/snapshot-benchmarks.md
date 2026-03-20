# Snapshot Format — Design Decisions & Benchmarks

## Summary

The snapshot format uses **sequential writes** with **buffered inline BLAKE3 hashing**
via a custom `std::streambuf`. Per-section hashes are computed during writes with
zero post-write overhead. Per-section BLAKE3 hashes are stored for future
BLS-signed snapshot verification (see `docs/snapshot-serving-plan.md`).

## Final Results (33 GB, realistic Vaulta/EOS mainnet distribution, release build)

Benchmark uses the same section distribution as a real EOS mainnet snapshot:
- `key_value_object`: 260M rows (~20 GB, 50.7%)
- `index256_object`: 360M rows (~16 GB, 40%)
- `index64_object`: 13M rows (~0.3 GB)
- 20+ smaller sections

| | Integrity Hash | Write | Read |
|---|---|---|---|
| **Spring (no hash)** | — | 70.0s (476 MB/s) | 254.4s (131 MB/s) |
| **Wire (inline BLAKE3)** | 34.1s | 63.3s (527 MB/s) | 226.4s (148 MB/s) |

- **Integrity hash**: 34s — pure serialization + BLAKE3 with no file I/O.
  Produces the same root hash as a snapshot write, useful for periodic
  verification without writing a full snapshot file.
- **Write**: **10% faster** — the 1 MB `hashing_streambuf` consolidates many
  tiny `fc::raw::pack` calls (1-8 bytes each) into bulk I/O, more than
  compensating for the BLAKE3 hash overhead. The ~29s of file I/O overhead
  (write − ihash) includes flushing 33 GB to disk.
- **Read**: **11% faster** — indexed section lookup eliminates Spring's linear
  scan through the file to find each section.
- **Added value**: per-section BLAKE3 hashes + root hash for future signed
  snapshot verification, per-section hash verification on load, indexed section
  lookup

Note: Run-to-run variance is ~10-30% due to disk cache state. Spring and Wire
benchmarks use identical data sets and were run back-to-back on the same machine.

## Design Evolution

The following approaches were explored during development. All measurements below
used an earlier, simpler benchmark (uniform kv-only distribution, ~38 GB). The
absolute throughput numbers are from that earlier benchmark — the relative
conclusions still hold.

### 1. Parallel Write with Temp Files (rejected)

**Approach:** Each section written to a separate temp file by N threads. `finalize()`
assembles temp files into the final output with a section index and per-section hashes.

**Why rejected:** The temp file assembly requires copying the entire snapshot
from temp files into the final output. This extra I/O pass negates
the parallelism gains from multi-threaded serialization.

### 2. Parallel Write with Sharded Contract Tables (rejected)

**Approach:** Split contract table index types across N shards by `table_id` range.
Each shard becomes a separate section (e.g., `key_value_object:0`, `:1`, ..., `:7`).

**Why rejected:**
- Writes: Still needed temp file assembly (same I/O overhead)
- Reads: Shards for the same index type must be read sequentially (chainbase
  isn't thread-safe for concurrent inserts to the same index), so sharding
  adds complexity without read benefit

### 3. Sequential Write + Parallel Post-Write SHA-256 Hash (rejected)

**Approach:** Write sections sequentially, then hash each section in parallel by
re-reading from the file (data is hot in page cache).

**Why rejected:** SHA-256 adds ~20s hash overhead. Replaced by BLAKE3.

### 4. Sequential Write + Unbuffered Inline BLAKE3 Hash (rejected)

**Approach:** Wrap the ostream to feed each write call directly to the BLAKE3
hasher with no intermediate buffer.

**Why rejected:** `fc::raw::pack` does many tiny writes (1-8 bytes per field).
Calling into the hasher for each write individually adds significant per-call
overhead (~50% slower than bulk hashing). This led to the buffered approach in
approach 6.

### 5. Sequential Write + Post-Write BLAKE3 Hash (superseded)

**Approach:** Write sequentially, then re-read and hash each section in parallel
using a thread pool.

**Results:** Worked well but had two issues:
- **Thread utilization**: With ~25 sections but 2 holding 90%+ of data
  (`key_value_object` 50%, `index256_object` 40%), most threads sat idle.
  The "8 hash threads" were effectively 1-2 threads for real workloads.
- **UBSAN overflow**: `sync_threaded_work` used `std::chrono::years::max()`
  in a `wait_for()` call, which overflows when converted to nanoseconds.
- **Re-read overhead**: Required reading 33+ GB back from the page cache.

Superseded by approach 6.

### 6. Sequential Write + Buffered Inline BLAKE3 Hash (chosen)

**Approach:** Interpose a custom `std::streambuf` (`hashing_streambuf`) between
`fc::raw::pack` and the file. The streambuf accumulates writes in a 1 MB buffer;
when the buffer fills, it hashes the contents with BLAKE3 and flushes to the
underlying file streambuf.

**Why chosen:**
- **Zero post-write overhead**: no re-read pass, no thread pool
- **Faster than no-hash baseline**: the 1 MB buffer coalesces many tiny pack
  writes into efficient bulk I/O, more than compensating for BLAKE3 CPU cost
- **Simplest implementation**: single-threaded, no temp files, no thread pool
- Per-section hashes enable future BLS-signed snapshot verification
- Fixes the UBSAN overflow (no more `sync_threaded_work`)
- BLAKE3 via LLVM's bundled `llvm-c/blake3.h` (no new dependency)

### Other optimizations tested

- **copy_file_range()**: Kernel-space file copy. Helped at 500 MB, hurt at 38 GB
  (page cache pressure at scale).
- **Tee wrapper** (single serialization to both file + hash): Slower due to
  per-field overhead with tiny writes.
- **Buffer-based single serialization**: Pack to memory buffer, then write
  buffer to both destinations. Slower due to size calculation pass overhead.

## File Format

```
[Header]  (8 bytes)
  magic:        uint32_t  (0x57495245 "WIRE")
  version:      uint32_t  (1)

[Section Data]
  section 0 raw packed rows
  section 1 raw packed rows
  ...

[Section Index] (num_sections entries, sorted by section name)
  name:         null-terminated string
  data_offset:  uint64_t  (from start of file)
  data_size:    uint64_t
  row_count:    uint64_t
  hash:         char[32]  (BLAKE3 of section row data)

[Footer]  (44 bytes)
  num_sections: uint32_t
  root_hash:    char[32]  (BLAKE3 of concatenated section hashes)
  index_offset: uint64_t  (byte offset where section index starts)
```

## Key Design Points

- **Section index at end of file:** Enables single-pass sequential writes since
  section sizes/hashes aren't known until all data is written.
- **Indexed section lookup:** O(num_sections) lookup vs Spring's O(N) scan from
  current file position.
- **Two-level write buffering:**
  - **`hashing_streambuf` (1 MB):** Custom `std::streambuf` interposed between
    `fc::raw::pack` and the file. Coalesces tiny writes into bulk hash + I/O
    operations. Used by the snapshot writer.
  - **`blake3_encoder` (64 KB):** Internal buffer that coalesces small writes
    before calling the LLVM hasher. Writes ≥ 64 KB bypass the buffer to avoid
    double-buffering when called from `hashing_streambuf`. Benefits all BLAKE3
    callers including the integrity hash writer.
- **Inline BLAKE3 hashing:** Hash computed incrementally as data flows through
  the buffer — no post-write re-read pass needed.
- **BLAKE3 via LLVM:** Uses `llvm-c/blake3.h` from LLVM's bundled implementation
  (already linked via chain library). No new external dependency.
- **Per-section hash verification on load:** Reader re-hashes each section from
  the memory-mapped file and compares against stored hashes.
