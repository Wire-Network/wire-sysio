# The KV storage seam

Everything outside the engine reaches KV rows and secondary index entries through `kv_store`
(`libraries/chain/include/sysio/chain/kv_store.hpp`). Contracts see the same behavior whichever engine sits behind it.

Two engines exist:

- **Chainbase** (`libraries/chain/kv_store.cpp`) is the only production engine.
- **`std::map` plus an undo log** (`libraries/testing/kv_map_store.cpp`) lives in the test library, which `nodeop` does
  not link. It exists to pin down the contract below. `SYSIO_TEST_KV_ENGINE=map` puts it behind a tester's chain, and
  `controller::testing_set_kv_store` is the only way in. No option or config field reaches it.

## Who uses it

| Caller | What it needs |
|---|---|
| `apply_context.cpp` | Point get, insert, update and erase on rows and entries, cursors for the iterator intrinsics |
| `controller.cpp` | Key-ordered iteration for snapshots and integrity hashes, bulk insert when loading a snapshot, row counts |
| `create_deltas.cpp` | Per-block deltas, and key-ordered iteration for a full history snapshot |
| `chain_plugin.cpp`, `chain_plugin.hpp`, `table_read.cpp` | Point gets and cursors in both directions, across tables for `get_table_by_scope` |
| `trx_priority_db.cpp` | A point get and a forward cursor |
| `deep_mind.cpp` | Rows by value, from the caller |
| `tester` and the contract test fixtures | Point gets, cursors, and a few direct writes |

`unittests/kv_tests.cpp`, parts of `unittests/kv_comprehensive_tests.cpp`, the performance cases in
`unittests/snapshot_tests.cpp` and `benchmark/kv_benchmark.cpp` still use `kv_object` directly. They test the chainbase
engine's own objects, so they belong on that side of the seam.

## The contract

**Order.** Rows sort by (code, table_id, key) and entries by (code, table_id, sec_key, pri_key). Keys compare as
unsigned bytes and a key sorts before the keys it prefixes. An entry's two keys compare one after the other. Joining
their bytes would give a different order: ("a", "z") sorts before ("ab", "a").

**Views.** `kv_row` and `kv_index_entry` hold `string_view`s into the engine, valid until the next write to it. An
engine that cannot hand out stable memory has to keep what it returned alive until then.

**Writes.** `insert_row` and `insert_index_entry` report a taken key and change nothing. `update_row`,
`update_index_entry`, `erase_row` and `erase_index_entry` take a row that `find_row`, `find_index_entry` or a cursor
returned, so that an engine can skip a second search. `engine_ref` carries whatever it needs for that. An engine may
leave it null and search again by the key in the row.

**Cursors.** A cursor walks one engine's whole order, across codes and tables, with one end position past the last row:

- A new cursor is at end. `next` from the last row and `prev` from the first row land on end, `next` at end stays there,
  and `prev` at end lands on the last row.
- A cursor sees the store as it stands when it moves, including writes made since its last move.
- A write that erases the row under a cursor, or changes its sort position, leaves that cursor unusable until its next
  seek. Any other write leaves it on its row.
- Each thread needs its own cursor. Read-only transactions run on several threads, so an engine keeps no shared mutable
  state on its read paths.

**Undo.** There is one undo stack for the whole chain state, in `chainbase::database`, and KV writes must roll back
together with everything else. An engine joins it through `join_undo_sessions`. Chainbase is already part of it. Any
other engine registers an `abstract_index` with `add_undo_participant`, as the transaction dedup does, and then gets
`add_undo_session`, `undo`, `squash`, `commit` and `set_revision` in lockstep with every other table.

**Deltas.** `row_deltas` and `index_entry_deltas` describe the top undo session as one entry per key, in key order: the
row as it now stands, or as it stood before an erase. A key that ends the session as it began has no delta.

**Snapshots.** There is no snapshot call. Key-ordered iteration is a cursor from the lowest position, and a load is
`insert_row` under `reserve_rows`. The two sections load on separate threads, so an engine must take row writes and
entry writes at the same time during a load.

## Decisions and trade-offs

**Dynamic dispatch.** `kv_store` and `kv_cursor` are abstract classes, so each KV intrinsic pays one or two indirect
calls. In two rotations against the same chain without the seam, a few operations at 10M rows moved past the
pass-to-pass spread, by up to 3.9%, and they were different operations each time. The bare `kv_it_read` host call moved
in both, by 1 to 2 ns. A build with the cursors devirtualized moved no operation at 10M rows past the spread and
measured that call at 22 ns where the same run gave 23 ns, so dispatch is about 1 ns of it, and the rest is building a
`kv_row` from the cursor. A compile-time seam would save that 1 ns and cost the one test binary that runs both engines.
docs/kv-baseline.md has the numbers.

**A cursor per iterator slot.** A slot opens its cursor on first use and keeps it when the slot is reused, so an action
allocates at most one per slot it touches, plus one spare that lets `kv_idx_find` search without taking a slot. An
action that uses no iterator pays nothing.

**Position stays in the slot.** The saved key bytes, the prefix and the end state remain in `kv_iterator_slot_common`,
because iterator position is defined by key at the intrinsic level for every engine. The cursor is only a way to step
without a search. When a write takes the row away, `forget_row` marks the slot and the next step seeks by key.
`forget_row` matches on the key instead of a row address, because an address means nothing on the far side of the seam.

**No upsert in the interface.** Per-row billing means every write has to read the old row first for its size and payer,
so an upsert is `find_row` followed by `insert_row` or `update_row`, and `kv_set` does that. A blind upsert would have
nothing to offer an engine.

**The cursor leaves room for a disk engine.** All cursor state belongs to the engine. One over an in-memory overlay for
reversible blocks on top of data on disk holds both positions and its merge state, and resolves tombstones as it steps.
It has to honor "sees the store as it stands", which is why the contract spells that out: an overlay position must
survive inserts, and a cached disk lookahead must be checked against the overlay when it is used. `seek_past_table` and
the whole-store walk need nothing from a B-tree that other ordered engines lack, and nothing here assumes pages, mmap or
a key encoding. The key size limits and the flat byte-ordered form of an entry key stay open questions for the choice of
engine.

**Cost of `clone`.** The read paths in `chain_plugin` and `table_read` need to look one row back and to remember a
position. They use `clone`, which is one allocation and an iterator copy on both current engines. A merged cursor has
more to copy, which is acceptable on those paths. The intrinsics never call it.

## State history changes

History deltas for the two KV tables now come from the seam, so their bytes no longer depend on chainbase's ids:

- Rows come in key order instead of id order, both in a block's deltas and in a full snapshot.
- A row erased and stored again under one key in a block is one delta, not two, and none if it ended as it began.
- An update that changed nothing has no delta.
- An index entry whose secondary key changed is a removal of the old position and a new entry at the new one. Before
  this, history reported only the new entry, so a client tracking the index could not tell that the old entry had gone.
- Within one table's deltas, removals come first and each group is in key order, so a client mirroring an index never
  holds an entry at its old and new positions at once.

Clients apply these deltas row by row and see the same final state. None of this reaches consensus: history logs are not
hashed by the chain.

## Testing the seam

`unittests/kv_store_tests.cpp` holds both engines under one database, so that one undo session drives both:

- A seeded random run of inserts, updates, erases, nested sessions, undo, squash, commit and cursor walks fails on the
  first difference between the engines. Its keys straddle `kv_key_blob`'s inline capacity and the integer comparison
  paths.
- Scripted cases pin the order, cursor edges, live cursors, taken keys and the delta rules.
- `iterators_match_a_model_by_key` drives the steps behind the iterator intrinsics, kept in `kv_iterator_steps.hpp` for
  this, on each engine against a `std::set` model that holds its position by key while writes land around it.
- `chains_on_both_engines_match` runs one chain on each engine. One validates the other's blocks, and the test compares
  history bytes per block, integrity hashes, snapshots, and each engine loading the snapshot the other wrote.

The KV suites also run with `SYSIO_TEST_KV_ENGINE=map` as ctest entries ending in `-kv-map`. A `validating_tester` keeps
its validator on chainbase, so a block the engines disagree on fails validation.
