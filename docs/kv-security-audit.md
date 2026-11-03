# KV Database Intrinsic Security Audit

This audit checks the 19 KV intrinsics against the known bugs in the database intrinsics of EOSIO, Leap and Spring, and
records the issues found in Wire's own implementation. `docs/kv-behavior-spec.md` states the behavior these findings rely
on, with a test for each rule.

## 1. Known upstream issues

### 1.1 Cross-contract write through a reused iterator (EOSIO #2208): critical

In the move to a new database API, the check that `table_obj.code == context.receiver` was left out of `db_update_i64`
and `db_remove_i64`. A contract could call `db_find_i64` on another contract's row, then pass that iterator to an update
or remove and change a row it did not own.

**KV: not possible.** No write takes a `code` argument or a handle. `kv_set`, `kv_erase`, `kv_idx_store`, `kv_idx_update`
and `kv_idx_erase` always use `receiver`, so there is no check to leave out. `kv_get`, `kv_it_create`, `kv_idx_create` and
`kv_idx_find` take a `code`, because reading another contract's tables is allowed.

### 1.2 Forged onerror (EOSIO #2671): high

Any user could deliver a fake `onerror` action to a contract after the handler moved out of the system contract.

**KV: does not apply.** This is an action dispatch issue, not a database one.

### 1.3 Use of an iterator after its row is removed: high

In the legacy `iterator_cache`, removing a row left the handle valid from WASM, and using it threw "dereference of
deleted object". The assert prevented a crash, but the behavior surprised contracts.

**KV: different design.** An iterator's position is a key, not a row. After the row is erased, `kv_it_read` and
`kv_idx_read` return `no_row`, and if a row is stored again at that key they read it back. Moves go strictly past the key.
Slots cache a pointer to the row for speed, and `kv_erase`, `kv_idx_erase` and `kv_idx_update` clear it through
`kv_iterator_pool::forget_row` for every open slot on that row. Nothing else can change a KV row while a slot is open,
because a handle lasts for one contract's execution and only that contract's code is running.

### 1.4 Chainbase iterators after a session rollback (EOSIO #9530): high

Rolling back a session freed nodes that iterators still pointed to.

**KV: does not apply.** A slot's cached row pointer cannot outlive a rollback. An undo happens only when the transaction
fails, which ends the contract that held the handle, and every handle is released before the next contract runs.

### 1.5 lower_bound at a table boundary (merge `0cb196b7fa`): medium

`db_lowerbound_i64` returned wrong results in edge cases at the boundary of a table's range.

**KV: covered.** A seek searches for (code, table_id, the greater of seek and prefix), and the landing row must have the
slot's code and table_id and start with its prefix, or the iterator is at `end`. A seek below the prefix starts from the
prefix, and one above it is `end`. An empty prefix covers one (code, table_id), not all of a contract's data.

### 1.6 Off by one in reverse iteration: medium

Upstream fixed a bug in the reverse step of its RocksDB backing store.

**KV: covered.** From `end`, `kv_step_prev` seeks to the byte successor of the prefix and steps back. An all-0xFF prefix
has no successor, so it seeks from the end of the (code, table_id) range instead. From the first row of the index it goes
to `end`, and it never moves out of the prefix.

### 1.7 Replay inconsistency in a RocksDB backing store (EOSIO #9632): medium

**KV: does not apply today.** State is in chainbase. The general lesson stands for any future engine: every state change
has to be deterministic and replayable.

### 1.8 abi_serializer stack overflow (CVE-2018-1000618): low

**KV: does not apply to the intrinsics,** which handle raw bytes. RPC reads that decode rows through an ABI use
`abi_serializer` with its `max_recursion_depth`.

### 1.9 RAM billing through a notification: high

A notified contract could bill data to the notifying contract or a third party.

**KV: covered.** `validate_account_ram_deltas` runs after every action. For an unprivileged receiver:

- A notified contract cannot grow another account's RAM at all.
- Otherwise an account whose RAM grew must have authorized the action, or it fails with
  `unauthorized_ram_usage_increase`.
- It must have authorized with `sysio.payer`, or it fails with `unsatisfied_authorization`. This is a Wire addition.

The payer must be named on insert: 0 is rejected with `invalid_table_payer`. On a row that exists, 0 keeps the payer.

`sysio.payer` is bypassed for a privileged `sysio.*` receiver, for a privileged receiver naming `sysio` as payer, and for
any action with `sysio` as an authorizing actor.

### 1.10 key_type enforcement on table_id_object (`9f7bd6e4b0`): low

**KV: does not apply.** There is one kind of key, a byte string, and no per-row format. A call that names a table takes
its `table_id` as a `uint32_t`, refused above 65535. Iterator moves take a handle and no `table_id`.

### 1.11 Foreign keys in snapshots (`0099d40555`): medium

Legacy rows referred to `table_id_object` rows, and snapshots had to respect that link.

**KV: does not apply.** `kv_object` is keyed by (code, table_id, key) and refers to nothing. Nothing links an index entry
to a row, and snapshots do no referential check. See [Orphaned index entries](#orphaned-index-entries).

## 2. Comparison with the legacy intrinsics

### Iterators

| Check | Legacy | KV |
|---|---|---|
| Handle bounds | Asserted in `iterator_cache` | Reserved bits, pool tag and an open slot, all checked |
| Row removed | Throws on use | `no_row` from a read, and moves go past the key |
| End of range | Asserted | `end`, with both sizes 0 |
| Pool size | Grows without limit | 1024 per pool, primary and secondary |

### RAM

| Check | Legacy | KV |
|---|---|---|
| Read-only transaction | Writes throw | Same, on all five writes |
| Payer | Must be named | Must be named on insert, and 0 keeps it on update |
| Notify context | `validate_account_ram_deltas` | Same |
| Payer consent | Authorization | Authorization, with `sysio.payer` |
| Row price | `billable_size_v` of the object | `key + value + 112`, and `sec_key + pri_key + 112` for an index entry. Protocol constants |

### Sizes

| Check | Legacy | KV |
|---|---|---|
| Key | Fixed 8 bytes | `max_kv_key_size`, default 256, up to 512 |
| Value | RAM limits only | `max_kv_value_size`, default 256 KiB, up to 1 MiB |
| Secondary key | Fixed by type | `max_kv_secondary_key_size`, default 256, up to 512 |
| Prefix | None | 512, the absolute limit |
| Table namespace | 64-bit names | `table_id` up to 65535, then `kv_invalid_table_id` |
| Read buffers | Bounds checked by the VM | Same, through `span` and `aligned_ptr` |

`seek` has no size limit, and it needs none. It is not copied, and comparing it against a stored key stops at the end of
the shorter one, which is at most 512 bytes.

## 3. Issues found in the KV implementation

### Fixed

| Issue | Severity | Fix | Tests |
|---|---|---|---|
| An empty index key was accepted, and the snapshot writer refuses one. Any contract keeping such an entry stopped snapshots and integrity hashes on every node. | High | `kv_idx_store` and `kv_idx_update` throw `kv_empty_key` | `kv_intrinsic_probe_tests/zero_length_sec_key`, `zero_length_pri_key`, `zero_length_new_sec_key` |
| A duplicate or colliding index entry threw an untyped chainbase error. | Medium | The call returns `exists`, and the host checks before `db.modify`, which on a row written in the same session would have lost it | `kv_intrinsic_probe_tests/duplicate_index_store_returns_exists`, `colliding_index_update_returns_exists`, `colliding_update_keeps_existing_entries` |
| Iterator handles carried over into notified contracts. | Low | `exec_one` releases both pools first | `kv_intrinsic_probe_tests/notified_receiver_cannot_use_primary_handle`, `notified_receiver_cannot_use_secondary_handle`, `notified_receiver_starts_from_slot_zero` |
| Attested snapshot hashes depended on allocation order, which another engine cannot reproduce, and two chains with the same rows could disagree. | High | KV sections are written in key order | `snapshot_part2_tests/test_kv_insertion_order_does_not_change_integrity_hash` |
| RAM prices followed `sizeof`, so a layout or engine change could move them. | Medium | The overhead is a named protocol constant | `kv_api_tests/billing_create_row`, `billing_idx_create_row` |
| An oversized `table_id` reported `kv_key_too_large`. | Low | `kv_invalid_table_id` | `kv_intrinsic_probe_tests/table_id_above_uint16_rejected` |
| Deep mind did not report index entries, and KV RAM lines had no trace. | Low | `KV_IDX_OP` lines and named `RAM_OP` lines | `deep_mind_tests/deep_mind` |

### Open

| Issue | Severity | Note |
|---|---|---|
| `table_id` collisions | Low | The chain does not enforce uniqueness of a 16-bit `table_id`. CDT codegen rejects a collision within one ABI, and a contract calling the intrinsics directly has no such check. A collision stays inside one contract's own tables, since `code` partitions them. RPC and SHiP trust the ABI's `table_id`, so a contract with colliding tables can make its rows decode as the wrong type for readers. |
| Charge below the stored size | Low | The charge is a price, not what a row occupies. Keys of 8 to 24 bytes, held inside the row, bill above what they store, except for 2 bytes on a row whose value lands just past a rounding boundary. Keys under 8 bytes fall a few bytes short. Keys past 24 bytes need a blob and can bill less: a `checksum256` entry stores 12 bytes above its charge, and at the worst lengths, 25 and 33 bytes, an index entry stores 42 above its charge and a row 26. `docs/kv-ram-billing.md` has the measured table. It is bounded per row, and the price is a protocol constant, so this will not change by itself. |

### By design

#### Orphaned index entries

`kv_erase` removes the row only, and any index entries remain until they are erased with `kv_idx_erase`. The host has no
way to find them: it does not know which secondary `table_id` belongs to a table, and `pri_key` is opaque. CDT removes
entries before it erases a row, as `multi_index` always did.

A contract that calls `kv_erase` directly keeps paying for its own orphaned entries, and stale lookups can then return
primary keys that no longer exist. Both stay inside that contract. An orphaned entry cannot break a snapshot, which does
no referential check.

#### Full sizes with an offset

`kv_get` and `kv_it_read` report a value's full size whatever the offset, so a contract can size a buffer from any read.

#### Iterator create and destroy cycles

Creating and destroying iterators in a loop costs CPU, which is billed. A slot holds at most 512 prefix bytes and its
position, and there are 2048 slots at most, so the memory an action can hold this way is bounded.

## 4. CVE summary

| CVE | Description | Applies to KV |
|---|---|---|
| CVE-2018-1000618 | `abi_serializer` stack overflow | No, the intrinsics handle raw bytes |
| CVE-2022-27134 | `batdappboomx` access control in `transfer` | No, this is at contract level |

Most upstream security fixes were made through issue tracking with no CVE, so this table is not a measure of what upstream
fixed.

## 5. Test checklist

| Class | Tests |
|---|---|
| Writes cannot name another contract | The intrinsics have no such argument |
| Cross-contract read | `kv_api_tests/cross_contract_idx_read` |
| Row erased under an iterator | `kv_intrinsic_probe_tests/iterator_invalidated_by_erase`, `kv_api_tests/it_erased_row_invalidation` |
| Erased and stored again | `kv_intrinsic_probe_tests/primary_position_is_by_key`, `secondary_position_is_by_key`, `kv_api_tests/iterator_erase_reinsert_same_key` |
| Unauthorized payer | `kv_api_tests/payer_other_requires_auth`, `payer_change_unauth_fails` |
| Payer without `sysio.payer` | `kv_api_tests/payer_change_active_only_fails` |
| Authorized payer | `kv_api_tests/payer_change_same_size` |
| Privileged bypass | `protocol_feature_tests/privileged_kv_payer_bypass` |
| Notify context | `kv_api_tests/notify_self_pay_bills_receiver`, `notify_third_party_payer_fails` |
| Seek bounds | `kv_intrinsic_probe_tests/primary_moves_read_the_row`, `secondary_prefix_bounds_the_iterator` |
| Reverse from end and from the first row | `kv_intrinsic_probe_tests/primary_moves_read_the_row`, `it_prev_at_begin_transitions_to_end`, `kv_api_tests/iterator_prev_from_begin` |
| All-0xFF prefix, secondary | `kv_intrinsic_probe_tests/secondary_prefix_bounds_the_iterator` |
| Iterator pool limits | `kv_api_tests/it_pool_overflow_1025`, `kv_intrinsic_probe_tests/secondary_iterator_slot_exhaustion` |
| Fabricated and wrong-pool handles | `kv_intrinsic_probe_tests/reserved_bits_handle_rejected`, `primary_intrinsic_on_secondary_handle_rejected`, `no_handle_value_rejected` |
| Empty keys | `kv_intrinsic_probe_tests/kv_set_empty_key_rejected`, `kv_erase_empty_key_rejected`, `zero_length_sec_key` |
| Size limits | `kv_intrinsic_probe_tests/kv_set_oversize_key_rejected`, `kv_set_oversize_value_rejected`, `kv_idx_store_oversize_sec_rejected` |
| Read-only transactions | `kv_api_tests/read_only_trx_rejects_write`, `read_only_rejects_kv_erase` |
| Offsets past the end | `kv_intrinsic_probe_tests/it_value_offset_boundaries`, `read_offsets_with_short_buffers` |
| Hand-built snapshots | `snapshot_part2_tests/kv_snapshot_reader_enforces_row_limits` |
| Orphaned entries | `kv_intrinsic_probe_tests/dangling_secondary_after_primary_erase` |
