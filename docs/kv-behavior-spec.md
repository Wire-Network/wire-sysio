# KV Behavior Spec

What a KV storage engine must do so that contracts, RAM billing, snapshots and history stay the same when the engine
changes. Chainbase is the reference engine. This file describes the 19 approved KV intrinsics and the host code behind
them, as declared in `libraries/chain/include/sysio/chain/webassembly/interface.hpp`.

Every rule names the test that pins it, by the name `--run_test` takes. A templated case needs a trailing `*` there. A
name with no suite is in the same suite as the name before it. A rule marked **no test** was read from the code and has
nothing holding it yet, and [Rules with no test](#rules-with-no-test) lists them together.

| Test suite | Binary | Source |
|---|---|---|
| `kv_api_tests` | `unit_test` | [unittests/kv_api_tests.cpp](../unittests/kv_api_tests.cpp) |
| `kv_intrinsic_probe_tests` | `unit_test` | [unittests/kv_intrinsic_probe_tests.cpp](../unittests/kv_intrinsic_probe_tests.cpp) |
| `kv_limits_tests` | `unit_test` | [unittests/kv_intrinsic_probe_tests.cpp](../unittests/kv_intrinsic_probe_tests.cpp) |
| `kv_tests` | `unit_test` | [unittests/kv_tests.cpp](../unittests/kv_tests.cpp) |
| `kv_chainbase_object_tests` | `unit_test` | [unittests/kv_object_tests.cpp](../unittests/kv_object_tests.cpp). These write to chainbase directly, so they pin the object types and their order, and reach no host code |
| `kv_comprehensive_tests` | `unit_test` | [unittests/kv_comprehensive_tests.cpp](../unittests/kv_comprehensive_tests.cpp) |
| `intrinsic_value_tests` | `unit_test` | [unittests/intrinsic_value_tests.cpp](../unittests/intrinsic_value_tests.cpp) |
| `payer_choice_test` | `unit_test` | [unittests/payer_choice_test.cpp](../unittests/payer_choice_test.cpp) |
| `protocol_feature_tests` | `unit_test` | [unittests/protocol_feature_tests.cpp](../unittests/protocol_feature_tests.cpp) |
| `forked_tests_savanna` | `unit_test` | [unittests/forked_tests_savanna.cpp](../unittests/forked_tests_savanna.cpp) |
| `snapshot_part2_tests` | `unit_test` | [unittests/snapshot_tests.cpp](../unittests/snapshot_tests.cpp) |
| `test_state_history` | `unit_test` | [unittests/state_history_tests.cpp](../unittests/state_history_tests.cpp) |
| `deep_mind_tests` | `unit_test` | [unittests/deep_mind_tests.cpp](../unittests/deep_mind_tests.cpp) |
| `api_part1_tests` | `unit_test` | [unittests/api_tests.cpp](../unittests/api_tests.cpp) |
| `read_only_trx_tests` | `unit_test` | [unittests/read_only_trx_tests.cpp](../unittests/read_only_trx_tests.cpp) |

Terms:

- A **row** is a key and a value in a table. An **index entry** is a (secondary key, primary key) pair in a secondary
  index. Each belongs to a `code` account, has a `table_id` and has a `payer`.
- The **receiver** is the contract executing now. `table_id` is a uint16 namespace. CDT derives it from the table's name,
  but the chain treats it as an opaque number.
- A key is a sequence of bytes. **Empty** means zero bytes.

## 1. Order

| Rule | Tests |
|---|---|
| **O1.** Keys compare as unsigned bytes. At the first byte that differs the lower one sorts first, and when one key is a prefix of the other the shorter sorts first. | `snapshot_part2_tests/kv_snapshot_sections_are_in_key_order`, `kv_chainbase_object_tests/kv_object_ordering`, `kv_intrinsic_probe_tests/eight_byte_keys_walk_in_byte_order` |
| **O2.** Rows sort by (code, table_id, key). `code` compares as the name's uint64 value and `table_id` as a number. | `snapshot_part2_tests/kv_snapshot_sections_are_in_key_order` |
| **O3.** Index entries sort by (code, table_id, sec_key, pri_key). This is pair order, not the order of the two keys joined: ("a", "z") sorts before ("ab", "a"), though "az" sorts after "aba". | `snapshot_part2_tests/kv_snapshot_sections_are_in_key_order`, `kv_chainbase_object_tests/kv_secondary_index_ordering`, `kv_intrinsic_probe_tests/eight_byte_keys_walk_in_byte_order` |
| **O4.** A code and a table_id are a partition. No lookup, seek or move crosses from one (code, table_id) into another. | `kv_intrinsic_probe_tests/lookups_stop_at_the_edge_of_their_table_and_code`, `it_prev_from_end_with_nothing_in_prefix`, `it_prev_with_no_byte_successor`, `reads_follow_the_code_argument`, `kv_api_tests/table_id_isolation` |

## 2. Arguments and limits

Misuse throws and aborts the action. When a `kv_set` has both a bad `table_id` and a bad mode, the `table_id` is checked
first, so `kv_invalid_table_id` is what throws. When any other two checks on one call would both fail, which exception
comes first is not part of this spec.

| Rule | Tests |
|---|---|
| **A1.** A `table_id` above 65535 throws `kv_invalid_table_id` on every call that takes one. | `kv_intrinsic_probe_tests/table_id_above_uint16_rejected`, `table_id_above_uint16_rejected_on_every_call`, `kv_api_tests/table_id_overflow_rejected`, `intrinsic_value_tests/kv_table_ids_stop_at_the_top` |
| **A2.** No row has an empty key, so every key argument throws `kv_empty_key` when empty: in `kv_set`, `kv_erase`, `kv_get`, `kv_idx_store`, `kv_idx_update`, `kv_idx_erase` and `kv_idx_find`. | `kv_intrinsic_probe_tests/kv_set_empty_key_rejected`, `kv_erase_empty_key_rejected`, `zero_length_sec_key`, `zero_length_pri_key`, `zero_length_new_sec_key`, `intrinsic_value_tests/kv_keys_must_not_be_empty` |
| **A3.** A `prefix` or a `seek` is not a key, so either may be empty, and a `seek` may lie outside its prefix. `kv_idx_find` throws `kv_key_outside_prefix` on a `sec_key` that does not start with `prefix`. | `intrinsic_value_tests/kv_prefix_and_seek_may_be_empty`, `kv_idx_find_key_must_start_with_the_prefix` |
| **A4.** A value may be empty. | `kv_intrinsic_probe_tests/zero_length_value`, `kv_api_tests/empty_value`, `zero_value` |
| **A5.** A call that stores something over its limit throws: `kv_key_too_large` for a key, `kv_value_too_large` for a value, and `kv_secondary_key_too_large` for a secondary key. The primary key of an index entry is held to the key limit. A size equal to the limit is accepted. | `kv_intrinsic_probe_tests/kv_set_oversize_key_rejected`, `kv_set_oversize_value_rejected`, `kv_idx_store_oversize_sec_rejected`, `kv_idx_store_oversize_pri_rejected`, `kv_idx_update_oversize_new_sec_key_rejected`, `max_size_key_and_value_accepted`, `kv_api_tests/oversize_value_262145_update` |
| **A6.** The limits are `max_kv_key_size` (default 256), `max_kv_secondary_key_size` (default 256) and `max_kv_value_size` (default 256 KiB). Governance can set the key limits from 1 to 512 and the value limit from 1 to 1 MiB. | `kv_api_tests/max_key_256`, `oversize_key_257`, `max_value_262144_create`, `oversize_value_262145_create`, `kv_limits_tests/raised_limits_apply_to_every_call`, `each_limit_holds_its_own_keys`, `limits_stop_at_their_caps` |
| **A7.** A limit applies to the sizes a call stores, not to what is already stored. Lowering one leaves stored rows in place. A `kv_set` that passes a key or value over the limit throws, so a row whose key is over it can no longer be rewritten. A row whose value is over it accepts a `kv_set` with a value that fits, and an entry over a limit accepts a `kv_idx_update` with a new key that fits. `kv_erase` and `kv_idx_erase` still remove them. | `kv_limits_tests/lowered_limits_keep_existing_rows_readable` |
| **A8.** A `prefix` over 512 bytes throws `kv_key_too_large`. This is the absolute limit, whatever governance has set. | `kv_intrinsic_probe_tests/kv_it_create_oversize_prefix_rejected`, `kv_tests/kv_check_prefix_size_bounds` |
| **A9.** A `seek` has no size limit. CDT sends a 513-byte all-0xFF key to reach the end of a table, so a governance limit here would break it. | `kv_intrinsic_probe_tests/it_lower_bound_stays_in_the_prefix`, `lookups_take_a_key_of_any_size` |
| **A10.** A `kv_set` mode other than 0 (upsert), 1 (insert) or 2 (update) throws `kv_invalid_mode`. | `kv_intrinsic_probe_tests/kv_set_unknown_mode_rejected`, `intrinsic_value_tests/kv_set_modes_are_a_closed_set`, `intrinsic_value_tests/kv_set_checks_the_table_id_first` |
| **A11.** In a read-only transaction, `kv_set`, `kv_erase`, `kv_idx_store`, `kv_idx_update` and `kv_idx_erase` throw `table_operation_not_permitted`. Reads work. | `kv_api_tests/read_only_trx_rejects_write`, `read_only_rejects_kv_erase`, `read_only_rejects_kv_idx_store`, `read_only_rejects_kv_idx_update`, `read_only_rejects_kv_idx_erase`, `read_only_trx_allows_every_read`, `read_only_trx_tests/db_read_only_mode_test` |
| **A12.** A key a call only looks up has no size limit: `key` in `kv_get` and `kv_erase`, `pri_key` and `sec_key` in `kv_idx_erase`, `pri_key` and `old_sec_key` in `kv_idx_update`, and `sec_key` in `kv_idx_find`. With no row at that key the call is a plain miss, so a key over 512 bytes misses rather than throws. | `kv_intrinsic_probe_tests/lookups_take_a_key_of_any_size` |

## 3. Status values

A state miss is returned and misuse throws. Contracts branch on these values, so they are part of the consensus surface.

| Value | Name | Meaning |
|---:|---|---|
| 0 | `ok` | the write happened, or the iterator is on a row |
| 1 | `end` | the iterator is past the last row in its prefix |
| 2 | `no_row` | no row is stored at the key |
| 3 | `exists` | a row is already stored at the key |

| Call | Returns |
|---|---|
| `kv_set` | `ok`; `exists` in insert mode on a taken key; `no_row` in update mode on a free key |
| `kv_erase` | `ok`, `no_row` |
| `kv_idx_store` | `ok`; `exists` if the (sec_key, pri_key) pair is taken |
| `kv_idx_update` | `ok`; `no_row` if the entry does not exist; `exists` if the new pair is taken |
| `kv_idx_erase` | `ok`, `no_row` |
| `kv_get` | the value's full size, or -1 |
| `kv_it_create`, `kv_idx_create` | a handle, always |
| `kv_idx_find` | a handle, or -1 with no handle allocated when no entry has the key |
| `kv_it_next`, `kv_it_prev`, `kv_it_lower_bound` and the `kv_idx_` twins | `ok` or `end`, never `no_row` |
| `kv_it_read`, `kv_idx_read` | `ok`, `end`, `no_row` |

| Rule | Tests |
|---|---|
| **S1.** Each call returns what the table above says, and a call that returns a miss changes no state and bills nothing. | `kv_intrinsic_probe_tests/every_status_from_every_write`, `kv_erase_missing_key_returns_no_row`, `kv_idx_erase_missing_entry_returns_no_row`, `kv_idx_update_missing_entry_returns_no_row` |

## 4. Writes

| Rule | Tests |
|---|---|
| **W1.** Writes act on the receiver's tables. No write takes a `code` argument. | The intrinsics have no such argument |
| **W2.** Upsert creates or replaces. Insert creates and returns `exists` on a taken key, leaving the row as it was. Update replaces and returns `no_row` on a free key, creating nothing. | `kv_intrinsic_probe_tests/every_status_from_every_write` |
| **W3.** A key and a value may overlap in contract memory. Both are stored as they were before the call. | `kv_intrinsic_probe_tests/aliased_key_value_spans` |
| **W4.** Keys and values hold any bytes. | `kv_api_tests/binary_roundtrip_all_bytes` |
| **W5.** `kv_idx_update` changes an entry's secondary key in place and can change its payer. With equal old and new keys it returns `ok` when the entry exists. A missing entry returns `no_row` even if the new pair is taken. | `kv_intrinsic_probe_tests/index_update_to_same_key_succeeds`, `index_update_edges`, `test_state_history/test_deltas_contract_index_kv_values` |
| **W6.** A duplicate store and a colliding update return `exists` and the action can carry on. Both entries survive, unchanged, and RAM does not move. This must hold for entries written earlier in the same transaction. In chainbase a failed create leaves the database as it was, so `kv_idx_store` does not check first. A failed modify loses a row the session already wrote, so `kv_idx_update` checks for the collision first. | `kv_intrinsic_probe_tests/duplicate_index_store_returns_exists`, `colliding_index_update_returns_exists`, `colliding_update_keeps_existing_entries` |
| **W7.** Rows and index entries have no link. `kv_erase` leaves index entries alone, an entry can outlive its row and can still be read, updated and erased, and `kv_idx_store` does not check that a primary key exists anywhere. CDT keeps them in step. | `kv_intrinsic_probe_tests/dangling_secondary_after_primary_erase`, `primary_key_after_primary_erased`, `cross_table_pri_key_permitted` |

## 5. Reads

| Rule | Tests |
|---|---|
| **R1.** `kv_get`, `kv_it_create`, `kv_idx_create` and `kv_idx_find` take a `code` and can read any contract's tables. A key found under one code is not found under another. Any uint64 is a `code`, and one with no account is a plain miss. | `kv_api_tests/cross_contract_idx_read`, `kv_intrinsic_probe_tests/kv_get_finds_a_key_only_under_its_own_code`, `intrinsic_value_tests/kv_reads_take_any_code` |
| **R2.** A zero-size buffer skips that copy. A short buffer truncates, and nothing is written past it. When a row is found, the size reported is the field's full size either way. | `kv_intrinsic_probe_tests/read_offsets_with_short_buffers`, `primary_moves_read_the_row`, `secondary_moves_read_the_entry`, `kv_tests/kv_copy_field_bounds` |
| **R3.** Offsets exist on value reads only, in `kv_get` and `kv_it_read`. An offset at or past the end copies nothing and still reports the full size. `kv_it_read` always reads the key from byte 0. | `kv_intrinsic_probe_tests/read_offsets_with_short_buffers`, `it_value_offset_boundaries` |
| **R4.** When a call does not return `ok`, both sizes are written as 0 and nothing is copied. The size out-parameters are always written. | `kv_intrinsic_probe_tests/primary_moves_read_the_row`, `iterator_end_state_reads`, `iterator_invalidated_by_erase`, `secondary_position_is_by_key` |
| **R5.** Copies run key first, then value, then the sizes. This shows only if a contract passes overlapping buffers. Two size pointers that overlap are refused with `wasm_exception`, "pointers not allowed to alias", because a misaligned pointer is written back as the call returns, in an order each compiler picks, so the bytes left in memory could differ between a clang build and a gcc build. An exact alias is refused too. | `kv_intrinsic_probe_tests/aligned_overlapping_read_outputs_copy_in_call_order`, `moves_copy_the_second_field_over_the_first`, `read_rejects_overlapping_size_pointers` |

## 6. Iterators

An iterator's position is a key, or a (sec_key, pri_key) pair for a secondary iterator. It is not a row. The rules are
the same for both families unless a rule says otherwise. `prefix` and `seek` apply to the key, and to `sec_key` for a
secondary iterator.

| Rule | Tests |
|---|---|
| **I1.** A seek lands on the first row at or above the seek key, within the prefix. A seek below the prefix starts from the prefix, an empty seek is the first row, and a seek above the prefix is `end`. | `kv_intrinsic_probe_tests/primary_moves_read_the_row`, `secondary_prefix_bounds_the_iterator` |
| **I2.** A secondary seek compares `sec_key` only, so it lands on the lowest `pri_key` under a matching `sec_key`. | `kv_api_tests/idx_duplicate_sec_keys` |
| **I3.** `kv_it_create` and `kv_idx_create` seek and allocate a handle whatever they land on. They read nothing. | `kv_intrinsic_probe_tests/lower_bound_empty_table_no_slot_leak`, `lower_bound_past_end_nonempty` |
| **I4.** A move past either edge of the prefix reaches `end`, even with rows just outside. | `kv_intrinsic_probe_tests/secondary_prefix_bounds_the_iterator`, `kv_api_tests/it_prefix_isolation` |
| **I5.** Next at `end` stays at `end`. Prev from `end` lands on the last row in the prefix. Prev from the first row goes to `end`. An all-0xFF prefix has no byte successor, and prev from `end` still finds its last row. | `kv_intrinsic_probe_tests/iterator_end_state_reads`, `it_prev_at_begin_transitions_to_end`, `primary_moves_read_the_row`, `secondary_prefix_bounds_the_iterator` |
| **I6.** Moves read the row they land on, into the buffers they were given. | `kv_intrinsic_probe_tests/primary_moves_read_the_row`, `secondary_moves_read_the_entry` |
| **I7.** A read returns the row stored at the position now, or `no_row` if there is none. A row erased and stored again at that key reads back, and a value written through reads as the new value. | `kv_intrinsic_probe_tests/primary_position_is_by_key`, `secondary_position_is_by_key`, `iterator_invalidated_by_erase` |
| **I8.** A move from a position with no row goes strictly past that key, in both directions, even if a row has since been stored there. | `kv_intrinsic_probe_tests/primary_position_is_by_key`, `kv_api_tests/iterator_next_after_erase_reinsert_advances`, `idx_iterator_next_after_remove_restore_advances` |
| **I9.** `kv_idx_update` moves an entry, and iterators on it stay at the old position: a read returns `no_row`, and next goes to the neighbor of the old key rather than following the entry. A refused update moves nothing. An update that keeps the key leaves iterators on the entry. | `kv_intrinsic_probe_tests/secondary_position_is_by_key`, `update_keeping_the_key_leaves_iterators_on_the_entry` |
| **I10.** `kv_idx_find` opens a handle on the first entry with exactly that `sec_key`, bounded by `prefix` for later moves. It returns -1 with no handle when no entry has the key. A key that lacks the prefix throws, as A3 says. | `kv_intrinsic_probe_tests/secondary_prefix_bounds_the_iterator`, `idx_find_miss_no_slot_leak` |
| **I11.** An iterator at `end` has no position, so a row stored later does not move it. A read and next stay at `end`. Prev and a seek look at the rows stored now and can find it. | `kv_intrinsic_probe_tests/row_stored_while_an_iterator_sits_at_end` |

## 7. Handles

A handle is an int32 that contracts can read, store and branch on, so its value is part of the consensus surface. An
engine does not choose it.

| Bits | Meaning |
|---|---|
| 0 to 9 | slot number |
| 10 to 15 | reserved, must be zero |
| 16 | 1 for a secondary handle, 0 for a primary one |
| 17 to 31 | reserved, must be zero |

| Rule | Tests |
|---|---|
| **H1.** There are two pools of 1024 slots each, one per family. The next handle past that throws `kv_iterator_limit_exceeded`. | `kv_api_tests/it_pool_overflow_1025`, `kv_intrinsic_probe_tests/iterator_slot_exhaustion`, `secondary_iterator_slot_exhaustion`, `kv_tests/kv_iterator_pool_independent_exhaustion` |
| **H2.** A new handle takes the lowest free slot in its pool. | `kv_tests/kv_iterator_pool_lowest_free_slot`, `kv_iterator_handle_encoding` |
| **H3.** A handle lasts for one contract's execution. Every handle is released before the next notified contract runs, and an inline action starts with empty pools, so each starts from slot 0 and an old handle is invalid. | `kv_intrinsic_probe_tests/notified_receiver_starts_from_slot_zero`, `notified_receiver_cannot_use_primary_handle`, `notified_receiver_cannot_use_secondary_handle`, `inline_action_iterator_isolation` |
| **H4.** A closed slot, a handle from the other pool, a reserved bit and -1 all throw `kv_invalid_iterator`. | `kv_intrinsic_probe_tests/use_after_destroy_rejected`, `double_destroy_rejected`, `reserved_bits_handle_rejected`, `primary_intrinsic_on_secondary_handle_rejected`, `secondary_intrinsic_on_primary_handle_rejected`, `no_handle_value_rejected`, `kv_tests/kv_validate_handle_dispatch`, `intrinsic_value_tests/kv_handles_are_checked_on_every_call` |

## 8. RAM

A row's charge is key size + value size + 112. An index entry's is sec_key size + pri_key size + 112. 112 is a protocol
price (`config::kv_row_overhead_ram_bytes`, `config::kv_index_entry_overhead_ram_bytes`). It does not follow the object
layout, so an engine that stores rows in fewer bytes charges the same.

| Rule | Tests |
|---|---|
| **B1.** Creating a row or entry charges the named payer in full. | `kv_api_tests/billing_create_row`, `billing_idx_create_row`, `idx_billing_store` |
| **B2.** An update with the same payer bills the size difference, and nothing for a same-size update. | `kv_api_tests/billing_update_grow`, `billing_update_shrink`, `billing_update_same_size`, `idx_billing_update_size_change`, `idx_billing_update_same_size` |
| **B3.** A payer change refunds the old payer its full old charge and charges the new payer its full new charge. | `kv_api_tests/payer_change_same_size`, `payer_change_with_growth`, `payer_change_with_shrink`, `idx_update_payer_change` |
| **B4.** Erasing refunds the row's current payer in full. | `kv_api_tests/billing_erase_refund`, `erase_refunds_correct_payer`, `idx_remove_refunds_payer`, `payer_choice_test/kv_idx_erase_refunds_entry_payer` |
| **B5.** Payer 0 keeps the current payer when the row exists, and throws `invalid_table_payer` when the call would create one. On a taken key, insert with payer 0 returns `exists`. | `kv_intrinsic_probe_tests/kv_set_payer_zero_keeps_payer_in_every_mode`, `kv_set_upsert_payer_zero_rejected_on_new_row`, `kv_set_insert_payer_zero_rejected_on_new_row`, `kv_api_tests/payer_zero_insert_rejected` |
| **B6.** After each action, for an unprivileged receiver, an account other than the receiver whose RAM grew must have authorized the action, or the action fails with `unauthorized_ram_usage_increase`. It must have done so with `sysio.payer`, or the action fails with `unsatisfied_authorization`, unless `sysio` is an authorizing actor on the action. A notified contract cannot grow another account's RAM at all. | `kv_api_tests/payer_other_requires_auth`, `payer_change_unauth_fails`, `payer_change_active_only_fails`, `payer_change_old_payer_unauth_ok`, `notify_self_pay_bills_receiver`, `notify_third_party_payer_fails`, `protocol_feature_tests/kv_sysio_actor_waives_sysio_payer_permission` |
| **B7.** A payer that is not an account aborts the transaction with `invalid_table_payer`, unless it is the receiver. On a new row or entry the check runs after the write, so the abort undoes the write. | `protocol_feature_tests/kv_payer_with_no_account_aborts`, `kv_intrinsic_probe_tests/write_rejects_payer_with_no_account` |

## 9. Undo

| Rule | Tests |
|---|---|
| **U1.** Every KV write belongs to the enclosing undo session. Undoing a transaction restores what it created, updated and erased, rows and index entries alike, along with the RAM it billed. | `kv_tests/kv_writes_undo_with_the_transaction`, `kv_same_size_writes_undo_with_the_transaction`, `kv_payer_changes_undo_with_the_transaction`, `kv_stacked_writes_undo_with_the_transaction`, `kv_writes_undo_with_the_block`, `kv_intrinsic_probe_tests/abort_in_an_inline_action_undoes_the_action_before_it`, `abort_in_a_notified_contract_undoes_both_contracts`, `refused_ram_bill_undoes_finished_writes`, `forked_tests_savanna/fork_switch_undoes_kv_writes` |

An engine needs nested sessions, because a transaction and a block each have one, and reversible blocks stay undoable
until they are irreversible. An action has no session of its own: an aborted inline action or a failed notification
aborts the whole transaction.

## 10. Snapshots

| Rule | Tests |
|---|---|
| **N1.** The KV sections hold rows in order O2 and entries in order O3, so identical state gives identical section bytes and hashes whatever order it was written in. Other sections still follow row ids. | `snapshot_part2_tests/test_kv_insertion_order_does_not_change_integrity_hash`, `kv_snapshot_sections_are_in_key_order`, `kv_sections_do_not_depend_on_transaction_order` |
| **N2.** A row holds code, payer, key, value and table_id, in that order. An index entry holds code, payer, sec_key, pri_key and table_id. Neither holds an id. | `snapshot_part2_tests/test_compatible_versions` |
| **N3.** The reader throws `snapshot_validation_exception` on an empty key, sec_key or pri_key, on a key, sec_key or pri_key over 512 bytes, and on a value over 1 MiB. Those are the absolute limits, since the governance limits may have been higher when a row was written. | `snapshot_part2_tests/kv_snapshot_reader_enforces_row_limits` |
| **N4.** The reader does not require key order. A loaded chain writes key order again, so the hash of a file out of order will not match the hash of the same state written back. | `snapshot_part2_tests/kv_snapshot_reader_enforces_row_limits`, `kv_snapshot_out_of_order_loads_to_the_same_state` |
| **N5.** Two rows or entries with the same key fail the load with `snapshot_validation_exception`. | `snapshot_part2_tests/kv_snapshot_reader_enforces_row_limits` |

## 11. State history

SHiP reports rows in `contract_row_kv` (code, payer, table_id, key, value) and index entries in `contract_index_kv`
(code, payer, table_id, sec_key, pri_key). Neither carries an id. Deltas are per block, and a storage engine supplies
them through `row_deltas` and `index_entry_deltas`, one entry per key in key order.

| Rule | Tests |
|---|---|
| **D1.** A store gives one present delta with the exact bytes. An update that keeps the key gives one present delta with the row as it now is. An erase gives one delta with `present` false and the row it removed. | `test_state_history/test_deltas_contract_row_kv_values`, `test_deltas_contract_index_kv_values` |
| **D2.** An index entry whose `sec_key` differs from the one the block started with gives two deltas: `present` false with the old pair, then a present delta with the new pair. A payer change keeps the pair and gives one present delta. | `test_state_history/test_deltas_contract_index_kv_exact`, `test_deltas_contract_index_kv_values` |
| **D3.** A delta is net of the block, against the state before it. Created and erased in one block gives nothing. Created and changed gives one present delta with the final bytes. Changed and erased gives one removal with the bytes the block started with, not the bytes last written. | `test_state_history/test_deltas_kv_are_net_of_the_block` |
| **D4.** A delta is per key. A row or entry that ends the block as it began gives none, whether it was rewritten with the same bytes and payer, moved and moved back, or erased and stored again. Erased and stored again with new bytes gives one present delta. | `test_state_history/test_deltas_kv_are_net_of_the_block`, `test_deltas_contract_index_kv_exact` |
| **D5.** Within one table of a block, removals come first and present rows after them, each in key order. A client mirroring an index never holds an entry at its old and new positions at once. | `test_state_history/test_deltas_kv_removals_come_first_in_key_order`, `test_deltas_contract_index_kv_exact` |

## 12. Deep mind

Deep mind lines are not consensus, but consumers parse them, so their format is a contract. `DEEP_MIND_VERSION` is
`wire_sysio 1 1`. Lines start with the field after `DMLOG`, and keys and values are hex.

```
KV_OP INS <action> <payer> <code> <table_id> <key> <value>
KV_OP UPD <action> <old_payer>:<payer> <code> <table_id> <key> <old_value>:<value>
KV_OP REM <action> <payer> <code> <table_id> <key> <value>
KV_IDX_OP INS <action> <payer> <code> <table_id> <pri_key> <sec_key>
KV_IDX_OP UPD <action> <old_payer>:<payer> <code> <table_id> <pri_key> <old_sec_key>:<sec_key>
KV_IDX_OP REM <action> <payer> <code> <table_id> <pri_key> <sec_key>
RAM_OP <action> <code>:<table_id>:<key> <family> <operation> <tag> <payer> <new_usage> <delta>
RAM_OP <action> <code>:<table_id>:<pri_key>:<sec_key> <family> <operation> <tag> <payer> <new_usage> <delta>
```

An empty value prints as nothing. An `INS` or `REM` line then ends in a space, and an `UPD` field reads `:<value>` or
`<old_value>:`, so split these lines on single spaces, not on runs of whitespace.

| Charge | Row: family `table_row` | Entry: family `secondary_index` |
|---|---|---|
| create | `add`, `primary_index_add` | `add`, `secondary_index_add` |
| update, same payer | `update`, `primary_index_update` | `update`, `secondary_index_update` |
| refund to the old payer | `remove`, `primary_index_update_remove_old_payer` | `remove`, `secondary_index_update_remove_old_payer` |
| charge to the new payer | `add`, `primary_index_update_add_new_payer` | `add`, `secondary_index_update_add_new_payer` |
| erase | `remove`, `primary_index_remove` | `remove`, `secondary_index_remove` |

| Rule | Tests |
|---|---|
| **L1.** A write logs its op line, then one RAM_OP line per charge: none for a same-size update, two for a payer change. On an index update that changes the key, the RAM_OP line names the new key. | `deep_mind_tests/deep_mind` |
| **L2.** A refused write logs nothing. | `deep_mind_tests/deep_mind` |
| **L3.** A transaction that fails keeps the lines it wrote, and nothing retracts them. `APPLIED_TRANSACTION` follows with `except` set in its trace, so a consumer has to drop that transaction's lines. | `deep_mind_tests/kv_lines_of_an_aborted_transaction_stay` |
| **L4.** A read-only or dry-run transaction logs nothing. | `deep_mind_tests/kv_dry_run_writes_log_nothing` |

## 13. Left to the engine

- **Ids.** Rows have an id in chainbase. No contract, snapshot, SHiP delta or deep mind line sees one, and loading a
  snapshot renumbers them. An engine needs none.
- **Read before write.** The size and payer of the old row decide what a write bills, so every update and erase reads
  the old row. An insert needs only to know that the key is free.
- **Key layout.** O3 is why a byte-ordered engine cannot simply join `sec_key` and `pri_key`: it needs a comparator, or
  an escape that keeps pair order.
- **Table_id collisions.** The chain does not enforce uniqueness of a `table_id`. Two tables that hash alike in one
  contract are one namespace to the host. CDT codegen rejects this within one ABI.

## Rules with no test

None.
