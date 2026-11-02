# KV baseline

Reference numbers for the KV pre-launch work. Every later step reports against this file.

The system contracts use `kv::table`, `kv::scoped_table`, `kv::global` and `kv::index`, and none declares a
`multi_index` table. `multi_index` is the compatibility shim, so `kv::table` is the main reference and comes first.

All numbers are the mean of three clean passes under sys-vm-oc in a Release build. Each pass averages 20 runs of each
operation. The ranges show the lowest and highest of the three pass averages.

The tables from here through Token transfers are the first baseline, from before the work on the chain started. Final
timing, at the end of this file, gives where the work ended up.

## `kv::table` per-operation time (`kv_table`)

Nanoseconds per operation. One run is 10,000 operations, or 10,000 rows for a scan. Host calls is the number of
intrinsic calls one operation makes.

| Operation | Host calls | 1M rows | 1M range | 10M rows | 10M range |
|---|---:|---:|---:|---:|---:|
| get | 1 | 1,233 | 1,229 to 1,237 | 1,692 | 1,691 to 1,693 |
| find | 6 | 2,095 | 2,067 to 2,132 | 2,595 | 2,588 to 2,604 |
| find + modify | 7 | 2,605 | 2,595 to 2,611 | 3,155 | 3,123 to 3,184 |
| modify by key | 2 | 1,754 | 1,745 to 1,762 | 2,226 | 2,218 to 2,233 |
| emplace | 2 | 1,670 | 1,651 to 1,695 | 2,195 | 2,169 to 2,219 |
| find + erase | 10 | 2,677 | 2,640 to 2,721 | 3,316 | 3,282 to 3,366 |
| erase by key | 2 | 1,414 | 1,388 to 1,427 | 1,934 | 1,915 to 1,948 |
| primary scan, per row | 3 | 1,334 | 1,312 to 1,368 | 2,028 | 1,963 to 2,104 |
| secondary scan (`kv::index`), per row | 3 | 2,537 | 2,500 to 2,556 | 4,048 | 3,990 to 4,085 |
| `kv::scoped_table` primary scan, per row | 3 | 1,343 | 1,312 to 1,384 | 1,994 | 1,977 to 2,019 |
| `kv::global` get | 1 | 50 | 49 to 52 | 50 | 48 to 54 |

Going from 1M to 10M rows costs 1.2x to 1.6x per operation, except `kv::global` get, which does not change.

Populating 1M rows took 2.2 s, and 10M rows took 26.7 to 27.5 s. That is both tables and the secondary index.

## `kv::table` against `multi_index`

Both sides come from the same three passes and the same `benchmark` binary. Each pass ran `kv_table`, then `kv_ops`. The
`multi_index` columns are that control run. They are not the table further down, which is from an earlier session. The
ratio is `kv::table` divided by `multi_index`.

| Operation | 1M `kv::table` | 1M `multi_index` | 1M ratio | 10M `kv::table` | 10M `multi_index` | 10M ratio |
|---|---:|---:|---:|---:|---:|---:|
| get | 1,233 | 1,423 | 0.87 | 1,692 | 2,001 | 0.85 |
| find | 2,095 | 2,481 | 0.84 | 2,595 | 3,149 | 0.82 |
| find + modify | 2,605 | 3,134 | 0.83 | 3,155 | 3,833 | 0.82 |
| emplace | 1,670 | 2,791 | 0.60 | 2,195 | 3,465 | 0.63 |
| find + erase | 2,677 | 4,188 | 0.64 | 3,316 | 5,075 | 0.65 |
| primary scan, per row | 1,334 | 1,504 | 0.89 | 2,028 | 2,161 | 0.94 |
| secondary scan, per row | 2,537 | 3,123 | 0.81 | 4,048 | 4,810 | 0.84 |

Populating took 4.4 s at 1M rows and 52.3 to 52.6 s at 10M rows in `multi_index`, about twice `kv::table`.

The two sides store the same row, but not the same key. A `kv::table` key is the 8 byte id. A `multi_index` key is
16 bytes, with the scope in front.

## `multi_index` per-operation time (`kv_ops`)

Nanoseconds per operation. One run is 10,000 operations, or 10,000 rows for a scan.

| Operation | 1M rows | 1M range | 10M rows | 10M range |
|---|---:|---:|---:|---:|
| get | 1,409 | 1,376 to 1,436 | 1,990 | 1,975 to 2,021 |
| find | 2,520 | 2,474 to 2,554 | 3,176 | 3,109 to 3,221 |
| find + modify | 3,184 | 3,116 to 3,228 | 3,903 | 3,895 to 3,912 |
| emplace | 2,837 | 2,777 to 2,884 | 3,532 | 3,496 to 3,567 |
| find + erase | 4,287 | 4,218 to 4,365 | 5,162 | 5,078 to 5,227 |
| erase, derived | 1,767 | | 1,987 | |
| primary scan, per row | 1,469 | 1,467 to 1,473 | 2,124 | 2,118 to 2,136 |
| secondary scan, per row | 2,915 | 2,912 to 2,922 | 4,485 | 4,317 to 4,656 |

Going from 1M to 10M rows costs 1.2x to 1.5x per operation.

Populating 1M rows took 4.5 to 4.6 s, and 10M rows took 52.9 to 54.8 s. That is both tables and the secondary index.

`find + erase` is what the benchmark measures, because `multi_index::erase` needs an iterator. The derived row subtracts
`find`. It is likely a low estimate of erase: the find inside `operase` looks up a row the matching `opemplace` run just
stored, so that find is likely cheaper than the `find` row above.

## Host call cost

`ophostcall` opens one primary iterator and calls `kv_it_read` on it 1,000,000 times per run, with no buffers. That host
call validates the handle, reads the row under the cursor and writes the key and value sizes through two pointers. It
does no tree search and copies no key or value (`apply_context::kv_it_read`).

These are the final numbers. Final timing has how they compare with base.

| Nanoseconds | 1M rows | 10M rows |
|---|---:|---:|
| host call (`kv_it_read`) | 23 | 23 |
| `kv::table` primary scan, per row | 342 | 422 |
| `kv::table` secondary scan, per row | 1,295 | 1,951 |
| `multi_index` primary scan, per row | 603 | 700 |
| `multi_index` secondary scan, per row | 1,448 | 2,123 |

The benchmark truncates each run to a whole nanosecond per call, so treat the host call as about 23 ns. It does not
change with table size, as expected for a call that does no search.

A `kv::table` primary scan step makes one host call, `kv_it_next`, which returns the key and the value. A secondary step
makes two, `kv_idx_next` and `kv_get`. A `multi_index` step makes two as well: `kv_it_next` or `kv_idx_next`, then
`kv_get`. At 23 ns per call that is 6.7% of a `kv::table` primary step at 1M rows and 5.5% at 10M, and 3.6% and 2.4% of
a secondary step. For `multi_index` it is 7.6% and 6.6% of a primary step, and 3.2% and 2.2% of a secondary step.

These shares are lower bounds, because the calls in a scan step also copy a key and a value, and this one does not.

`kv_it_read` replaced `kv_it_status`, which the first passes measured at 6 ns. The two do not compare: `kv_it_status`
passed no pointers and read no row.

## Access layers (`kv_contract`)

Microseconds for one action over the whole table. Raw calls the intrinsics directly. Shim is `multi_index`.

| Operation | Rows | KV Raw | KV Shim | Shim/Raw |
|---|---:|---:|---:|---:|
| Populate | 100 | 25 | 78 | 3.2 |
| Find All | 100 | 9 | 63 | 6.7 |
| Iterate All | 100 | 13 | 44 | 3.3 |
| Update All | 100 | 22 | 91 | 4.1 |
| Erase All | 100 | 21 | 105 | 5.1 |
| Populate | 500 | 96 | 380 | 3.9 |
| Find All | 500 | 37 | 313 | 8.5 |
| Iterate All | 500 | 41 | 210 | 5.1 |
| Update All | 500 | 98 | 459 | 4.7 |
| Erase All | 500 | 71 | 528 | 7.5 |

## Token transfers (`kv_token`)

Microseconds for one action. Each transfer is two reads and two writes. Fast is `multi_index` on a trivially copyable
row.

| Transfers | KV Shim | KV Raw | KV Fast | Shim/Raw | Fast/Raw |
|---:|---:|---:|---:|---:|---:|
| 10 | 20 | 9 | 20 | 2.2 | 2.2 |
| 50 | 66 | 27 | 64 | 2.4 | 2.4 |
| 100 | 118 | 49 | 120 | 2.4 | 2.5 |
| 500 | 504 | 187 | 509 | 2.7 | 2.7 |

## What the numbers mean

These notes describe the baseline passes. The intrinsics have changed since, so `find` and scans now make fewer host
calls.

- **The by-key rows are the reference for `kv::table` writes.** A search of the 97 `modify` calls in the system
  contracts found none through an iterator, and they erase both by key and through iterators. `find + modify` and `find
  + erase` are there to compare with `multi_index`.
- **`find` is the costly way to read one row.** `get` makes one search of the key tree. `find` makes three, in
  `kv_contains`, `kv_it_create` and `kv_it_lower_bound`, and then an id lookup for the value.
- **`find + erase` uses `std::move`.** `erase` takes its iterator by value, so a plain `erase(it)` copies it first,
  which adds a `kv_it_create` and a `kv_it_lower_bound`. Erasing through an iterator also moves it to the next row and
  decodes that row.
- **`bench_kv_table` stores a row with a string.** Every read decodes it through a datastream, as in `bench_kv_ops`. A
  trivially copyable row takes a plain copy in `kv::table`, and the table above does not cover that path.
- **`kv::global` get reads one row over and over.** Its path through the tree stays in the CPU caches, so this is the
  cost of the wrapper and `kv_get` with no cache misses. One run takes about 0.5 ms, so its range is wider.
- **The scoped table shows no cost of its own.** All its rows are in one scope, and its ranges overlap those of the
  plain primary scan. All primary rows of all tables share one tree, so it has its own chain with the same number of
  rows. Storing a third table on the main chain would deepen the tree under every other operation.
- **Rows are visited in an order unrelated to how they were stored.** `bench_kv_ops` and `bench_kv_table` store rows by
  a permuted id, so key order differs from allocation order, and they visit them in a second order unrelated to both.
  Reading rows in the order they were stored touches adjacent memory on every step and hides the cache misses.
- **Point operations are never served from a CDT row cache.** Each `multi_index` operation builds its own table object.
  `kv::table` has no row cache.
- **`emplace` lands at random tree positions.** Even ids hold rows and odd ids stay free. Each `emplace` run is undone
  by the matching erase run, so the row count stays constant.
- **`multi_index` scan cost includes the CDT row cache.** A scan keeps one table object, so each row step also decodes
  the row and adds it to `_items`. A `kv::table` scan step decodes the row into the iterator and keeps nothing else.
- **A range is the noise floor within one session.** A change smaller than the range is not shown by these numbers. In
  the `kv::table` table the widest range is primary scan at 10M rows, at 7.2%, and the rest are within 5.5%, leaving out
  `kv::global` get. In the `multi_index` table the widest is secondary scan at 10M rows, at 7.9%, and the rest are
  within 4.4%, leaving out the host call, which is truncated.
- **Sessions differ by more than a range.** The control run used the same `bench_kv_ops.wasm` as the `multi_index`
  table, with a relinked `benchmark`. Its point operations came within 2.3% of that table and its primary scan within
  2.4%, but its secondary scan was 7.1% higher at 1M rows and 7.2% higher at 10M. The cause is not known. To compare
  against this file, use the same commands on the same core with at least three clean passes, and run `kv_table` and
  `kv_ops` side by side in that session, so the control shows how far the session itself moved.

## How it was measured

| | |
|---|---|
| Commit | the `multi_index` tables: the commit that adds this file, on top of `fae750b37b`. The `kv::table` tables: the commit that adds them, on top of `9c7f21610f` |
| Build type | Release, clang-18 |
| WASM runtime | sys-vm-oc (`sys_vm_oc`, the benchmark default) |
| wire-cdt | `187d6a24` |
| `bench_kv_ops.wasm` sha256 | `cdf214732168ee626eb3fbc66c62cffb0ebbc0e6116f46c8b59339b7ebd1cb86`, in both sessions |
| `bench_kv_table.wasm` sha256 | `18806323ec1deb0e90dd9cfb895bfcecc57ddcba4fb3bfcefd6f4a8260542faf` |

The `multi_index` passes ran on a build from shortly before their commit. After them, literals in the benchmark host
code became named constants and comments were added. The timed code, which is the chain and the contract WASM, did not
change.

Build:

```bash
cmake -B build/release -S . -G Ninja -DCMAKE_BUILD_TYPE=Release \
   -DCMAKE_C_COMPILER=/usr/bin/clang-18 -DCMAKE_CXX_COMPILER=/usr/bin/clang++-18 \
   -DBUILD_SYSTEM_CONTRACTS=ON -DBUILD_TEST_CONTRACTS=ON -Dcdt_DIR=$WIRE_CDT_DIR \
   -DBUILD_OPP_BUNDLES=OFF -DDISABLE_WASM_SPEC_TESTS=ON \
   -DENABLE_CCACHE=ON -DENABLE_DISTCC=OFF -DENABLE_TESTS=ON \
   -DCMAKE_TOOLCHAIN_FILE=$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build/release --target benchmark test_contracts_project
```

Run from `build/release`, three times each:

```bash
taskset -c 4 benchmark/benchmark -f kv_table -r 20 --kv-rows 1000000 10000000
taskset -c 4 benchmark/benchmark -f kv_ops -r 20 --kv-rows 1000000 10000000
taskset -c 4 benchmark/benchmark -f kv_contract -r 20
taskset -c 4 benchmark/benchmark -f kv_token -r 20
```

Every pass ran on a quiet machine, with no builds or tests running. Disturbed passes were thrown out and run again.

## Final timing

Where the pre-launch work ended up. The build type, runtime, core and commands are the ones above, and so is the
quiet-machine rule. All four builds ran in one session on the same contract WASMs, with each pass running every build in
turn, so drift in the session falls on all of them alike. That makes the builds easier to compare with each other than
with the tables above, which came from other sessions and an older interface.

| Column | Commit | What it adds |
|---|---|---|
| base | Report state memory per id in the KV benchmarks | Steps 1 to 5 |
| step 6 | Keep short KV keys inside the row | Keys of up to 30 bytes inside the row. These timings measured that layout, and the slot holds 24 now |
| step 7 | Put a storage seam between the chain and KV state | `kv_store`, see docs/kv-store-seam.md |
| final | Compare other KV key sizes a word at a time | The review fixes and the changes in the table further down |

| | |
|---|---|
| wire-cdt | `6a570a17` |
| `bench_kv_ops.wasm` sha256 | `f6d115533d3e6bf55ddfe9c0f9d86082946abaac1ea722f3301a6cc64a604110` |
| `bench_kv_table.wasm` sha256 | `63337fb11d8952de097be6462afe124820796d9a1f754d2646e7340c12a14148` |

### `kv::table`

Nanoseconds per operation. Change is final against base.

| Operation | 1M base | 1M final | Change | 10M base | 10M step 6 | 10M step 7 | 10M final | 10M final range | Change |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| get | 1,138 | 994 | -13% | 1,594 | 1,413 | 1,421 | 1,422 | 1,393 to 1,449 | -11% |
| find | 1,145 | 948 | -17% | 1,574 | 1,402 | 1,381 | 1,392 | 1,362 to 1,418 | -12% |
| find + modify | 1,718 | 1,347 | -22% | 2,099 | 1,875 | 1,917 | 1,830 | 1,802 to 1,855 | -13% |
| modify by key | 1,654 | 1,471 | -11% | 2,107 | 1,946 | 1,925 | 1,855 | 1,835 to 1,888 | -12% |
| emplace | 1,235 | 1,012 | -18% | 1,760 | 1,492 | 1,483 | 1,473 | 1,470 to 1,478 | -16% |
| find + erase | 1,277 | 1,152 | -10% | 1,786 | 1,613 | 1,597 | 1,590 | 1,584 to 1,597 | -11% |
| erase by key | 1,025 | 798 | -22% | 1,450 | 1,235 | 1,284 | 1,245 | 1,225 to 1,277 | -14% |
| primary scan, per row | 355 | 342 | -4% | 449 | 413 | 419 | 422 | 406 to 446 | -6% |
| secondary scan (`kv::index`), per row | 1,615 | 1,295 | -20% | 2,481 | 1,930 | 1,935 | 1,951 | 1,919 to 1,976 | -21% |
| `kv::scoped_table` primary scan, per row | 368 | 359 | -2% | 445 | 437 | 417 | 430 | 426 to 438 | -3% |
| `kv::global` get | 50 | 52 | +5% | 49 | 52 | 51 | 53 | 51 to 56 | +7% |

### `multi_index`

| Operation | 1M base | 1M final | Change | 10M base | 10M step 6 | 10M step 7 | 10M final | 10M final range | Change |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| get | 1,355 | 982 | -28% | 1,899 | 1,415 | 1,429 | 1,439 | 1,427 to 1,458 | -24% |
| find | 1,870 | 1,446 | -23% | 2,521 | 1,905 | 1,913 | 1,894 | 1,866 to 1,909 | -25% |
| find + modify | 2,506 | 1,863 | -26% | 3,162 | 2,437 | 2,530 | 2,389 | 2,366 to 2,423 | -24% |
| emplace | 2,086 | 1,550 | -26% | 2,740 | 2,039 | 2,061 | 2,058 | 2,032 to 2,092 | -25% |
| find + erase | 3,256 | 2,482 | -24% | 3,950 | 3,002 | 3,031 | 3,008 | 2,959 to 3,053 | -24% |
| primary scan, per row | 644 | 603 | -6% | 733 | 701 | 696 | 700 | 686 to 709 | -5% |
| secondary scan, per row | 1,853 | 1,448 | -22% | 2,754 | 2,111 | 2,158 | 2,123 | 2,111 to 2,140 | -23% |
| host call (`kv_it_read`) | 20 | 23 | +13% | 20 | 19 | 21 | 23 | 22 to 23 | +15% |

Against the first baseline at 10M rows, `kv::table` get went from 1,692 ns to 1,422, find from 2,595 to 1,392, primary
scan from 2,028 to 422 and secondary scan from 4,048 to 1,951. Those come from different sessions, so treat them as
rough.

### Memory and populate

An id is two rows and one index entry. Keys now sit in the row, so the three table types come out the same.

| | Base | Final |
|---|---:|---:|
| `kv::table` state memory, bytes per id at 10M rows | 456.3 | 488.2 |
| `kv::scoped_table` state memory, bytes per id at 10M rows | 464.3 | 488.2 |
| `multi_index` state memory, bytes per id at 10M rows | 480.3 | 488.2 |
| `kv::table` populate, 10M rows, seconds | 21.3 to 23.5 | 18.3 to 18.5 |
| `multi_index` populate, 10M rows, seconds | 37.7 to 38.0 | 27.4 to 27.5 |

### Small tables

Microseconds, shown in whole numbers by the benchmark. Most of these searches stay in the CPU caches, so they show the
cost of the code more than the cost of memory.

| Operation | Rows | KV Raw base | KV Raw final | KV Shim base | KV Shim final |
|---|---:|---:|---:|---:|---:|
| Populate | 100 | 24 | 26 | 57 | 56 |
| Find All | 100 | 9 | 11 | 49 | 49 |
| Iterate All | 100 | 6 | 7 | 31 | 33 |
| Update All | 100 | 21 | 22 | 76 | 71 |
| Erase All | 100 | 19 | 21 | 79 | 79 |
| Populate | 500 | 95 | 95 | 276 | 257 |
| Find All | 500 | 37 | 39 | 258 | 237 |
| Iterate All | 500 | 18 | 19 | 160 | 153 |
| Update All | 500 | 95 | 94 | 403 | 360 |
| Erase All | 500 | 75 | 74 | 410 | 375 |

| Transfers | KV Shim base | KV Shim final | KV Raw base | KV Raw step 6 | KV Raw step 7 | KV Raw final | KV Fast base | KV Fast final |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 10 | 15 | 14 | 8 | 11 | 10 | 9 | 15 | 13 |
| 50 | 51 | 50 | 26 | 33 | 32 | 27 | 50 | 49 |
| 100 | 97 | 88 | 46 | 59 | 58 | 46 | 96 | 86 |
| 500 | 410 | 374 | 191 | 242 | 239 | 185 | 405 | 368 |

KV Raw token keys are 24 bytes, which took the slow path of `kv_key_less`. Step 6 moved that path out of line, and raw
transfers lost about a quarter. Comparing a word at a time in that path took them back to base.

KV Raw Find All stayed about 2 us slower. Those reads use 8-byte keys in rows already in the cache, the same case as
`kv::global` get.

### Step 6: short keys in the row

Kept. The test was a win at 10M rows beyond the pass-to-pass spread on point reads or scans, with nothing slower beyond
the spread. Against base at 10M rows, step 6 made every `kv::table` and `multi_index` operation faster beyond the
spread, by 4% to 26%, with three exceptions. `kv::scoped_table` primary scan and the bare host call stayed inside the
spread, and `kv::global` get went from 49 ns to 52 ns.

`kv::global` get is the one operation that ended up slower: 2 to 4 ns on a read of one cached row. It comes from keeping
the 16-byte compare inline, which gave `multi_index` 3 to 4% on find + modify, emplace and find + erase at 10M rows. RAM
prices did not move. docs/kv-ram-billing.md gives what is stored against what is billed.

### Step 7: the seam

Dynamic dispatch stayed. Two rotations compared step 7 with step 6 at 10M rows. Each had a few operations slower past
the spread, by 1.6% to 3.9%, and they were different operations each time:

- In this one, `kv::table` find + modify, second-cycle emplace and erase by key, and `multi_index` find + modify.
- In the other, `multi_index` emplace and primary scan.

Erase by key moved 3.9% in one and 0.2% in the other. The bare `kv_it_read` call moved in both, by 1 to 2 ns.

A build with the cursors devirtualized, timed beside the same code, moved no operation at 10M rows past the spread, and
took `kv_it_read` from 23 ns to 22 ns. So dispatch is about 1 ns of that call, and the rest is building a `kv_row` from
the cursor. The review fixes took the call to 23 ns, and they add a check of the two size pointers to every read.

### Changes timed one at a time

Each was timed at 10M rows against the build before it.

| Change | Result | Decision |
|---|---|---|
| Save the iterator position lazily | Primary scan +0.8% and +1.0%, secondary scan -1.8% and +0.9%, all inside the spread | Dropped |
| `modify_unindexed`, which skips the index walk | find + modify -3% to -6% in four rotations | Kept |
| Insert-if-absent in `insert_row` | `multi_index` emplace -3.2%, then -1.3% on a repeat. `kv::table` emplace +1.2%, then +2.0% | Dropped |
| 16-byte compare inline | `multi_index` find + modify -2.8%, emplace -2.6%, find + erase -3.7%. `kv::table` get -4.1%, find -6.1%. `kv::global` get +4% | Kept |
| Value size in the row | Nothing slower beyond the spread, `kv::global` get -2%, no memory cost | Kept |
| Word-at-a-time compare for other key sizes | Raw token transfers 215 us to 185 us at 500. At 10M rows three operations moved past the spread, on paths it does not touch: `kv::table` emplace -2.0%, find + erase -1.3%, scoped primary scan +2.1% | Kept |
| 40-byte key slot, 38 bytes inline | 520.2 bytes per id, +6.6%. `kv::table` emplace +3.1%, find + erase +3.2%, secondary scan +6.4%. `multi_index` get +2.3%, find +2.9%, secondary scan +9.8% | Not adopted |

The benchmarks use 8-byte and 16-byte keys, so the 40-byte slot shows only its cost. None of them has a key of 31 to 38
bytes, where it would help.
