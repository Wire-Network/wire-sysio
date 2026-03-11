# Snapshot Format — Design Decisions & Benchmarks

## Summary

The snapshot format uses **sequential writes** with a **parallel BLAKE3 hash pass** in
`finalize()`, and **multi-threaded reads** across index types. Per-section BLAKE3
hashes are computed and stored for future BLS-signed snapshot verification
(see `docs/snapshot-serving-plan.md`).

## Final Results (38 GB, release, back-to-back on same machine)

| | Write | Read | Total |
|---|---|---|---|
| **Spring (no hash)** | 38.5s (1013 MB/s) | 77.6s (502 MB/s) | 116.1s |
| **Wire (BLAKE3)** | 48.2s (809 MB/s) | 67.4s (579 MB/s) | 115.6s |

- **Write**: ~25% slower due to BLAKE3 parallel hash pass over 38 GB
- **Read**: ~15% faster (indexed section lookup vs Spring's linear scan)
- **Total**: essentially equivalent
- **Added value**: per-section BLAKE3 hashes + root hash for future signed
  snapshot verification, indexed section lookup

Note: Run-to-run variance is significant (~30%) due to disk cache state.

## Approaches Attempted

### 1. Parallel Write with Temp Files (rejected)

**Approach:** Each section written to a separate temp file by N threads. `finalize()`
assembles temp files into the final output with a section index and per-section hashes.

**Results (38 GB, release, 8 threads):**
- Write: 71.6s (545 MB/s)
- Read: 62.2s (627 MB/s)

**Why rejected:** The temp file assembly requires copying the entire snapshot
(38 GB) from temp files into the final output. This extra I/O pass negates
the parallelism gains from multi-threaded serialization.

### 2. Parallel Write with Sharded Contract Tables (rejected)

**Approach:** Split contract table index types across N shards by `table_id` range.
Each shard becomes a separate section (e.g., `key_value_object:0`, `:1`, ..., `:7`).

**Results:** Same as approach 1 — sharding didn't help because:
- Writes: Still needed temp file assembly (same I/O overhead)
- Reads: Shards for the same index type must be read sequentially (chainbase
  isn't thread-safe for concurrent inserts to the same index), so sharding
  adds complexity without read benefit

### 3. Parallel Write without Hashing (tested for measurement)

**Approach:** Same as #1 but with SHA-256 hashing disabled to measure hash overhead.

**Results (38 GB, release, 8 threads, solo run):**
- Write: 58.4s (668 MB/s) — hash adds ~13s (18%) to writes
- Read: 52.5s (744 MB/s)

**Conclusion:** Hashing adds meaningful overhead, but is required for the
snapshot signing/verification system.

### 4. Sequential Write with Inline Hash (1 thread, tested for measurement)

**Approach:** Same as #1 but with only 1 write thread to isolate threading benefit.

**Results (38 GB, release):**
- Write: 94.9s (411 MB/s) — confirms threading DID help (94.9s → 71.6s)

**Conclusion:** Multi-threaded serialization helps, but the temp file assembly
overhead negates most of the benefit.

### 5. Sequential Write + Parallel Post-Write SHA-256 Hash

**Approach:** Write sections sequentially to the output file (like Spring's
`ostream_snapshot_writer`), then hash each section in parallel by re-reading
from the file (data is hot in page cache). Index and footer appended at end.

**Results (38 GB, release):**
- Write: 59.2s (659 MB/s)
- Read: 68.0s (574 MB/s)

**Conclusion:** Good approach but SHA-256 adds ~20s hash overhead.

### 6. Sequential Write + Inline BLAKE3 Hash (rejected)

**Approach:** Same as #5 but compute BLAKE3 hash inline during writes using
a `hashing_ostream_wrapper` that feeds each byte to the hasher as it's written.

**Results (38 GB, release):**
- Write: 72.2s (541 MB/s)

**Why rejected:** `fc::raw::pack` does many tiny writes (1-8 bytes per field).
Even with BLAKE3's speed, the per-call overhead of intercepting each write
makes inline hashing ~50% slower than the post-write bulk approach.

### 7. Sequential Write + Parallel Post-Write BLAKE3 Hash (chosen)

**Approach:** Same as #5 but using BLAKE3 instead of SHA-256 for hashing.
BLAKE3 is ~3-5x faster than SHA-256, especially on large data.

**Results (38 GB, release):**
- Write: 48.2s (809 MB/s)
- Read: 67.4s (579 MB/s)

**Why chosen:**
- No temp files, no assembly copy — clean single-pass write
- BLAKE3 parallel hash pass re-reads from page cache (mostly CPU-bound)
- Only ~25% write overhead vs Spring's no-hash baseline
- Simplest implementation with best performance
- Per-section hashes enable future BLS-signed snapshot verification
- BLAKE3 available via LLVM's bundled implementation (no new dependency)

### Other optimizations tested (within approach 1)

- **copy_file_range()**: Kernel-space file copy instead of userspace buffer.
  Helped at 507 MB (301ms vs 369ms) but hurt at 38 GB (80.8s vs 71.6s).
  Likely due to page cache pressure at scale.
- **64 MB copy buffer** (vs 1 MB default): Marginal improvement.
- **Tee wrapper** (single serialization to both file + hash): Slower due to
  per-field overhead in `fc::raw::pack` (many small writes).
- **Buffer-based single serialization**: Pack to memory buffer, then write
  buffer to both destinations. Slower due to size calculation pass overhead.
- **4 shards vs 8 shards**: 8 shards faster (more parallelism outweighs
  extra temp file overhead).

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
- **8 hash threads:** Sections are distributed via atomic work-stealing.
- **4 MB hash read buffer:** Balances memory usage vs syscall overhead per thread.
- **BLAKE3 via LLVM:** Uses `llvm-c/blake3.h` from LLVM's bundled implementation
  (already linked via chain library). No new external dependency.
