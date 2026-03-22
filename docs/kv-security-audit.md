# KV Database Intrinsic Security Audit

## Background

This document catalogs known security vulnerabilities, bugs, and fixes related to
database intrinsic/host functions (db_store_i64, db_find_i64, db_update_i64,
db_remove_i64, db_get_i64, db_next_i64, db_previous_i64, db_lowerbound_i64,
db_upperbound_i64, db_end_i64, and all db_idx* secondary index functions) across
EOSIO, Leap, and Spring (AntelopeIO) — and evaluates whether each class of
vulnerability applies to Wire's new KV implementation.

---

## 1. Known Upstream Security Vulnerabilities

### 1.1 Cross-Contract Write via Iterator Reuse (EOSIO #2208) — CRITICAL

**What:** In the transition from old to new DB API, the check that
`table_obj.code == context.receiver` was accidentally omitted from
`db_update_i64` and `db_remove_i64`. A contract could use `db_find_i64` to
obtain an iterator to *another* contract's table row, then call `db_update_i64`
or `db_remove_i64` with that iterator to modify or delete data it did not own.

**Exploit:** Contract A calls `db_find_i64(B.code, scope, table, pk)` → gets
iterator `i`. Then calls `db_update_i64(i, A, new_buffer, size)` → overwrites
B's row.

**Fix:** Added `EOS_ASSERT(table_obj.code == context.receiver, table_access_violation,
"db access violation")` to `db_update_i64`, `db_remove_i64`, and all secondary
index `update`/`remove` methods.

**KV assessment: MITIGATED.** Our KV implementation uses `receiver` (not a
user-supplied `code` parameter) as the lookup key for all write operations
(`kv_set`, `kv_erase`, `kv_idx_store`, `kv_idx_remove`,
`kv_idx_update`). Since writes always query by `(receiver, key)`, a contract
cannot write to another contract's namespace. Read operations (`kv_get`,
`kv_contains`, `kv_it_create`, `kv_idx_find_secondary`, `kv_idx_lower_bound`)
accept a `code` parameter, which is the correct design (cross-contract reads are
permitted; cross-contract writes are not).

**Recommendation:** No action needed. The architectural choice of keying writes
by `receiver` structurally prevents this class of bug. Add a unit test that
explicitly verifies cross-contract write attempts fail.

---

### 1.2 onerror Handler Spoofing (EOSIO #2671) — HIGH

**What:** After the onerror handling was moved out of the system contract, any
user-signed transaction could deliver a fake `onerror` action to a contract
without going through the system contract. Contracts that trusted the onerror
payload could be tricked into performing unintended state changes.

**KV assessment: NOT DIRECTLY APPLICABLE.** This is an action-dispatch issue,
not a database intrinsic issue. However, any contract using KV storage inside
an `onerror` handler should validate the action source.

**Recommendation:** No KV-specific action needed.

---

### 1.3 Iterator Cache Use-After-Remove (Upstream Ongoing) — HIGH

**What:** In the legacy `iterator_cache` design, calling `db_remove_i64(iter)`
sets `_iterator_to_object[iter] = nullptr` but the integer handle remains valid
from the WASM side. A subsequent `db_get_i64(iter)` or `db_next_i64(iter)` on
the stale handle triggers `EOS_ASSERT(result, table_operation_not_permitted,
"dereference of deleted object")`.

While the assertion prevents a crash, the behavior is non-obvious and has led to
contract bugs.

**Upstream fix:** The `iterator_cache::get()` method checks for null pointer:
```cpp
auto result = _iterator_to_object[iterator];
EOS_ASSERT( result, table_operation_not_permitted, "dereference of deleted object" );
```

**KV assessment: DIFFERENT DESIGN, PARTIALLY APPLICABLE.** Our KV iterator pool
uses a `kv_iterator_slot` with an explicit `in_use` flag and `status` enum
(`iterator_ok`, `iterator_end`, `iterator_erased`). When a row pointed to by an
iterator is erased, `kv_it_key` and `kv_it_value` detect the deletion by
re-seeking the chainbase index and set `status = iterator_erased`. This is a
better design than the upstream pointer-nulling approach.

**HOWEVER: There is a subtle issue.** If the iterator's `current_key` still
matches a row that was erased and then a *new* row was inserted with the same
key (within the same transaction), the iterator would find the new row rather
than reporting `iterator_erased`. This is technically correct (cursor-like
semantics) but differs from the legacy behavior.

**Recommendation:**
1. Document the cursor-reseeking semantics explicitly.
2. Add a test: erase a row pointed to by an iterator, insert a new row with the
   same key, and verify the iterator finds the new row (not erased status).

---

### 1.4 Chainbase Iterator Invalidation on Session Rollback (EOSIO #9530) — HIGH

**What:** When chainbase sessions are rolled back (undo), all chainbase iterators
become invalid because the underlying B-tree nodes may be freed. The
`session-invalidate-iterators` branch added explicit invalidation of all
iterators when a session is undone.

**KV assessment: MITIGATED BY DESIGN.** Our KV iterators store `current_key`
bytes (not chainbase iterator pointers) and re-seek using
`idx.find(boost::make_tuple(code, sv_key))` on every access. This means a
session rollback that removes the pointed-to row will be detected at next use
(the find will return `end()`), and the iterator transitions to
`iterator_erased` or `iterator_end`.

**Recommendation:** Add a test that:
1. Creates a KV iterator pointing to a row
2. Triggers a session rollback (e.g. via failed inline action)
3. Verifies the iterator correctly reports `iterator_erased`/`iterator_end`

---

### 1.5 db_lowerbound_i64 Incorrect Results (ENF/Leap PR #720 lineage) — MEDIUM

**What:** The `db_lowerbound_i64` function had a bug in certain edge cases with
the RocksDB backing store where the lower_bound returned incorrect results when
the search key was at the boundary of a table's range. The merge commit
`0cb196b7fa` references "fix-db-lowerbound-i64-desc".

**KV assessment: NEEDS VERIFICATION.** Our `kv_it_lower_bound` implementation
uses `idx.lower_bound(boost::make_tuple(slot.code, seek_key))` and then
validates that the result has the correct prefix:
```cpp
if (itr != idx.end() && itr->code == slot.code && key_has_prefix(*itr, slot.prefix))
```
This should be correct, but the prefix boundary check deserves testing.

**Recommendation:** Add edge-case tests for:
- lower_bound at exact prefix boundary
- lower_bound with key == prefix (should find first entry)
- lower_bound with key beyond all entries in prefix range
- lower_bound with empty prefix (should iterate all of code's data)

---

### 1.6 db_previous_i64 Off-by-One (EOSIO RocksDB) — MEDIUM

**What:** Commit `bfbd46047b` fixed a one-line bug in `db_previous_i64` for the
RocksDB backing store, where the reverse iteration logic produced incorrect
results.

**KV assessment: NEEDS VERIFICATION.** Our `kv_it_prev` has complex logic for
both "at end" and "at valid position" cases, using `byte_successor` to compute
the upper bound of the prefix range. The `byte_successor` function for all-0xFF
prefix bytes (which produces an empty successor, meaning "end of code range")
needs careful testing.

**Recommendation:** Test `kv_it_prev` from:
- `iterator_end` state with entries in range → should find last entry
- `iterator_end` state with NO entries in range → should stay `iterator_end`
- First entry → should transition to `iterator_end`
- Middle entry → should find previous entry
- With prefix = `\xFF\xFF\xFF` (all-FF prefix edge case)

---

### 1.7 RocksDB DB Intrinsic Replay Inconsistency (EOSIO #9632) — MEDIUM

**What:** The RocksDB implementation of database intrinsics had bugs in replay
logic causing inconsistencies. Commit `5dfa382e3b` merged fixes for "DB
intrinsic replay logic".

**KV assessment: MITIGATED.** We use chainbase (shared memory), not RocksDB, for
KV storage. Chainbase's undo/redo mechanism is well-tested. However, the general
lesson applies: all state mutations must be deterministic and replayable.

**Recommendation:** Ensure that the new KV intrinsics are included in the
deep_mind logging (already done via `dm_logger->on_kv_set()` and
`dm_logger->on_kv_erase()`).

---

### 1.8 abi_serializer Stack Overflow (CVE-2018-1000618) — LOW (for KV)

**What:** EOSIO eos had a stack overflow in `abi_serializer` that could crash a
node by sending deeply nested ABI types.

**KV assessment: NOT DIRECTLY APPLICABLE.** The KV API deals with raw bytes, not
ABI-deserialized data. However, the `chain_plugin` RPC endpoints that read KV
data and deserialize it via ABI should enforce recursion limits.

**Recommendation:** No KV-specific action needed. Ensure chain_plugin KV query
endpoints use the existing `abi_serializer::max_recursion_depth` limits.

---

### 1.9 RAM Billing Manipulation via Notify Context (General Pattern) — HIGH

**What:** In the legacy API, a contract receiving a notification (where
`receiver != act->account`) could manipulate RAM billing by storing data with a
payer that is the notifying contract or a third party. The protection is:
```cpp
// In validate_account_ram_deltas():
if (!privileged && itr->delta > 0 && itr->account != receiver) {
   EOS_ASSERT(not_in_notify_context, unauthorized_ram_usage_increase, ...);
   EOS_ASSERT(has_authorization(itr->account), unauthorized_ram_usage_increase, ...);
}
```

**KV assessment: CORRECTLY HANDLED.** Our KV implementation:
1. `kv_set` always writes to `receiver`'s namespace
2. Payer defaults to `receiver`; explicit payer requires `privileged`
3. `validate_account_ram_deltas()` runs after every action execution and catches
   unauthorized RAM increases
4. Additional Wire-specific payer authorization checks enforce
   `config::sysio_payer_name` permission

**Recommendation:** Verify test coverage for:
- Non-privileged contract trying `payer != receiver` (should fail)
- Notify context RAM billing (should fail for non-self increases)
- Privileged contract setting arbitrary payer (should succeed)

---

### 1.10 key_type Enforcement on table_id_object (EOSIO, 2018) — LOW

**What:** Commit `9f7bd6e4b0` added `key_type` to `table_id_object` to enforce
consistent key usage — preventing a contract from accessing the same table with
different key types (e.g. using `db_idx64` operations on a table created with
`db_store_i64` only).

**KV assessment: NOT APPLICABLE.** The KV API has a single unified key format
(byte strings). The `key_format` field (0=raw, 1=standard) is stored per row
and does not affect lookup behavior.

**Recommendation:** No action needed.

---

### 1.11 Foreign Key / Snapshot Integrity (EOSIO, 2018) — MEDIUM

**What:** Commit `0099d40555` fixed how contract table objects were snapshotted
and integrity-checked. The issue was that `table_id_object` rows served as
foreign keys for `key_value_object` rows, and snapshot traversal needed to
respect this relationship to avoid orphaned or misattributed data.

**KV assessment: PARTIALLY APPLICABLE.** Our `kv_object` has no foreign key
dependency (it's keyed by `(code, key)` directly). However, `kv_index_object`
rows reference their parent `kv_object` via the same `(code, table, primary_key)`
tuple. **If a `kv_object` is erased without cleaning up its `kv_index_object`
entries, orphaned secondary index rows will remain.**

**THIS IS A REAL ISSUE IN OUR CODE.** Looking at `kv_erase()`:
```cpp
int64_t apply_context::kv_erase(const char* key, uint32_t key_size) {
   ...
   db.remove(*itr);  // Removes kv_object only
   return delta;      // Does NOT remove associated kv_index_object entries!
}
```
The `kv_erase` intrinsic removes the primary `kv_object` row but does **not**
clean up any associated `kv_index_object` rows. This is a data consistency bug.

**The CDT-side `wire::kv::table` is expected to call `kv_idx_remove` for each
secondary index before calling `kv_erase`.** However, a malicious contract
could call `kv_erase` directly via the raw intrinsic without cleaning up
secondary indices, leaving orphaned index entries that:
- Waste RAM (billed to the payer of the secondary index rows)
- Could cause stale secondary index lookups to return primary keys that no
  longer exist
- Could cause snapshot inconsistency if integrity checks expect referential
  consistency

**Recommendation: HIGH PRIORITY.**
1. **Option A (Enforce at intrinsic level):** Have `kv_erase` automatically
   remove all `kv_index_object` rows matching the erased key's primary key.
   This requires knowing the table/index structure, which the raw intrinsic
   does not have.
2. **Option B (Best-effort cleanup):** If `key_format == 1` (standard encoding
   with table:scope:pk embedded), extract the table and pk from the key and
   remove matching secondary index entries.
3. **Option C (Document and test):** Accept that the CDT library is responsible
   for cleanup, document this clearly, and add integrity-check logic that can
   detect orphaned secondary index rows.
4. **Option D (Hybrid):** Add a `kv_erase_with_indices(key, table)` intrinsic
   that does the cleanup atomically.

---

## 2. Vulnerability Classes and KV Assessment

### 2.1 Iterator Invalidation

| Check | Legacy (upstream) | Our KV |
|-------|-------------------|--------|
| Bounds check on iterator handle | `EOS_ASSERT(iterator >= 0 && < size)` | `kv_iterators.get(handle)` checks `in_use` flag |
| Deleted object dereference | Null pointer check in `iterator_cache::get()` | Re-seek by key; detects missing rows as `iterator_erased` |
| End iterator dereference | `EOS_ASSERT(iterator >= 0)` blocks negative values | `status == iterator_end` check in `kv_it_key/value` |
| Session rollback invalidation | Explicit invalidation added in EOSIO #9530 | Re-seek design is inherently safe |
| Iterator pool exhaustion | No limit (grows unbounded) | Fixed pool of `max_kv_iterators=16` with limit check |

**Overall: IMPROVED.** The re-seeking design and fixed pool are safer than upstream.

---

### 2.2 RAM Billing

| Check | Legacy (upstream) | Our KV |
|-------|-------------------|--------|
| Read-only transaction blocks writes | `EOS_ASSERT(!trx_context.is_read_only())` | Same check on `kv_set`, `kv_erase`, `kv_idx_*` |
| Empty payer rejected | `EOS_ASSERT(payer != account_name())` | Payer defaults to `receiver` when 0; never empty |
| Non-owner payer requires privilege | N/A (legacy uses `require_authorization`) | `SYS_ASSERT(privileged)` when payer != receiver |
| Notify context RAM protection | `validate_account_ram_deltas()` | Same function runs, same checks |
| Billable size accounting | `config::billable_size_v<key_value_object>` | `config::billable_size_v<kv_object>` with correct overhead |

**Overall: EQUIVALENT with additional Wire-specific payer permission checks.**

---

### 2.3 Cross-Contract Access

| Check | Legacy (upstream) | Our KV |
|-------|-------------------|--------|
| Write scoping | `table_obj.code == receiver` check (was missing, EOSIO #2208) | Writes always use `receiver` as code — structural guarantee |
| Read access | Any contract can read any table via `db_find_i64(code, ...)` | Same: `kv_get(code, ...)`, `kv_it_create(code, ...)` |
| Secondary index writes | `table_obj.code == context.receiver` | `kv_idx_store/remove/update` use `receiver` implicitly |

**Overall: STRUCTURALLY STRONGER.** The legacy API required a runtime check that
was once missing; our KV API prevents the bug by design.

---

### 2.4 Buffer Overflow / Size Validation

| Check | Legacy (upstream) | Our KV |
|-------|-------------------|--------|
| Key size limit | N/A (fixed 8-byte uint64_t) | `max_kv_key_size = 256` enforced |
| Value size limit | Implicit via RAM limits only | `max_kv_value_size = 256KB` enforced |
| Partial write overflow | N/A | `static_cast<uint64_t>(offset) + value_size <= max` (uint64 cast prevents uint32 wrap) |
| Secondary key size | N/A (fixed type-specific sizes) | `max_kv_secondary_key_size = 256` enforced |
| Iterator pool overflow | Unbounded growth | `max_kv_iterators = 16` with assertion |
| Read buffer overrun | WASM linear memory bounds checked by sys-vm | Same (legacy_span enforces bounds) |

**Overall: IMPROVED.** Explicit limits where legacy had none.

---

### 2.5 Undo/Rollback Consistency

| Check | Legacy (upstream) | Our KV |
|-------|-------------------|--------|
| Chainbase undo support | Built-in for all index types | Built-in for `kv_object` and `kv_index_object` |
| Value size undo amplification | Unbounded (could create huge undo entries) | `max_kv_value_size = 256KB` caps amplification |
| Iterator state after undo | Invalidated (EOSIO #9530) | Re-seek design handles transparently |

**Overall: EQUIVALENT with improvement from value size cap.**

---

## 3. Identified Issues in Our KV Implementation

### ISSUE-1: kv_erase Does Not Clean Up Secondary Index Entries — BY DESIGN

The `kv_erase` intrinsic removes only the `kv_object` row. Any `kv_index_object`
rows referencing the same primary key must be removed separately via `kv_idx_remove`.

**Status: Not a bug.** This is the same pattern as legacy `multi_index` where the
CDT wrapper calls `remove_secondaries()` before `db_remove_i64`. The host provides
primitives; the CDT library orchestrates cleanup. A contract calling raw `kv_erase`
without `kv_idx_remove` would leave orphaned secondaries, but that is a contract
bug, not a host bug.

**File:** `libraries/chain/apply_context.cpp` (kv_erase function)

### ISSUE-1a: Fixes Applied During This Audit

The following hardening was applied based on this audit:

1. **Empty key rejection** — `kv_set`, `kv_erase` now assert
   `key_size > 0`. An empty key could cause confusion with prefix iteration.
2. **key_format validation** — `kv_set` now asserts
   `key_format <= 1`. Values other than 0 (raw) or 1 (standard) are rejected.

---

### ISSUE-2: kv_it_key/kv_it_value Missing Offset Bounds Check (LOW)

In `kv_it_key`:
```cpp
if (dest_size > 0 && offset < itr->key_size) {
    uint32_t copy_size = std::min(dest_size, itr->key_size - offset);
    memcpy(dest, itr->key_data() + offset, copy_size);
}
```
This is correct — it silently returns 0 bytes copied if offset >= key_size.
However, `actual_size` is set to the full key size regardless of offset, which
could confuse callers. The same pattern exists in `kv_it_value`.

**Impact:** Low — the CDT library handles this correctly, but a raw intrinsic
user might be confused by `actual_size` not reflecting the offset.

**Recommendation:** Consider whether `actual_size` should be
`max(0, key_size - offset)` instead of `key_size`.

---

### ISSUE-3: No Rate Limiting on Iterator Create/Destroy Cycles (LOW)

A malicious contract could call `kv_it_create`/`kv_it_destroy` in a tight loop.
The pool size is limited to 16, so at most 16 iterators exist simultaneously.
However, the create/destroy cycle itself has no CPU cost beyond the intrinsic
call overhead.

**Impact:** Minimal. The existing CPU billing per intrinsic call provides
implicit rate limiting.

**Recommendation:** No action needed. The CPU billing model handles this.

---

### ISSUE-4: Billable Size Accounting for kv_erase (MEDIUM)

In `kv_erase`:
```cpp
int64_t delta = -static_cast<int64_t>(itr->key_size + itr->value.size()
                + config::billable_size_v<kv_object>);
```

This credits back the primary key row's billable size. But if there were
associated `kv_index_object` rows (secondary indices), their billable size is
NOT credited back because `kv_erase` does not remove them (ISSUE-1). This means
the payer of the secondary index rows continues to be billed for storage that
is effectively orphaned.

**Impact:** Economic — users pay for orphaned secondary index storage.

**File:** Same as ISSUE-1.

---

## 4. CVE Summary

| CVE | Description | Applies to KV? |
|-----|-------------|-----------------|
| CVE-2018-1000618 | abi_serializer stack overflow | No (KV uses raw bytes) |
| CVE-2022-27134 | batdappboomx access control in transfer | No (contract-level, not intrinsic) |

No CVEs were found specifically targeting EOSIO/Leap/Spring database host
functions. Most security fixes were made via internal issue tracking (EOSIO
GitHub issues #2208, #2671, etc.) without CVE assignment. The EOSIO project did
not systematically use CVEs for their security disclosures.

---

## 5. Priority Recommendations

| Priority | Issue | Status |
|----------|-------|--------|
| ~~HIGH~~ | ~~ISSUE-1: Orphaned secondary indices~~ | **By design** — CDT handles cleanup |
| ~~HIGH~~ | ~~ISSUE-5: Billable size from orphaned indices~~ | **By design** — same as above |
| **DONE** | Empty key rejection | Fixed: `key_size > 0` asserted in kv_set/kv_erase |
| **DONE** | key_format validation | Fixed: `key_format <= 1` asserted in kv_set |
| **DONE** | Section 1.5: kv_it_lower_bound edge cases | Tests: testitlbound, tstlbound |
| **DONE** | Section 1.6: kv_it_prev edge cases | Tests: tstprevbgn, tstpreveras, tstreviter |
| **DONE** | Section 1.3: Iterator after erase+reinsert | Test: tstreraseins |
| **DONE** | Read-only transaction write rejection | Test: tstrdonly |
| **LOW** | ISSUE-2: actual_size semantics with offset | CDT handles correctly |
| **LOW** | ISSUE-3: Iterator create/destroy cycles | No action (CPU billing handles it) |

---

## 6. Test Coverage Checklist

These tests should exist in the KV test suite to guard against the vulnerability
classes identified above:

- [x] Cross-contract write attempt via kv_set with different code → fails (testcrossrd)
- [x] Cross-contract read via kv_get with different code → succeeds (testcrossrd)
- [x] Iterator to erased row → reports iterator_erased (tsterasedinv)
- [x] Erase row, insert same key, iterator finds new row (tstreraseins)
- [x] kv_set with payer != receiver from non-privileged → fails (tstpayeroth)
- [x] kv_set with payer != receiver from privileged sysio.* → succeeds (tstpayerprv)
- [x] kv_it_lower_bound at exact key and gap (testitlbound, tstlbound)
- [x] kv_it_prev from iterator_end → finds last entry (tstreviter, tstrbegin)
- [x] kv_it_prev from first entry → transitions to end (tstprevbgn)
- [x] kv_it_prev on erased iterator → reports erased status (tstpreveras)
- [x] Iterator pool exhaustion (create 16+1 iterators) → exception (tstitexhfail)
- [x] Empty key (key_size=0) rejected by kv_set (tstemptykey)
- [x] Invalid key_format (>1) rejected by kv_set (tstbadfmt)
- [x] kv_set in read-only transaction → fails (tstrdonly)
- [x] RAM billing in notify context — notification receiver writes to own namespace (tstsendnotif → kvnotify)
- [x] Read-only transaction calling kv_set → table_operation_not_permitted (tstrdonly)
- [x] kv_it_key with offset >= key_size → returns 0 bytes copied (tstkeyoffbnd)
- [x] Secondary key size > max_kv_secondary_key_size → kv_secondary_key_too_large (tstbigseckey)
