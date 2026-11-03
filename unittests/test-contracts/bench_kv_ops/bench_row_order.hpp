#pragma once
/**
 * Row ids, secondary keys and payload shared by the `kv_ops` and `kv_table` benchmark contracts, so both store and
 * visit the same rows in the same orders.
 */
#include <cstdint>

namespace bench_row_order {

/// Prime, so `index * stride % total` reaches every slot once for any total it does not divide.
inline constexpr uint64_t store_stride = 2'654'435'761;

/// Prime, and its step stays far from adjacent in both key order and storage order at 100k, 1M and 10M rows.
inline constexpr uint64_t visit_stride = 1'001'818'183;

/// Odd, so multiplying is a bijection on uint64: secondary keys are unique and unrelated to primary key order.
inline constexpr uint64_t secondary_key_multiplier = 0x9E3779B97F4A7C15;

inline constexpr const char* row_payload = "benchmark_payload_data_here";

/// Id of the row stored for `index`.
inline uint64_t stored_id(uint64_t index, uint64_t total) { return (index * store_stride % total) * 2; }

/// Id of the row an operation visits for `index`.
inline uint64_t visited_id(uint64_t index, uint64_t total) { return stored_id(index * visit_stride % total, total); }

/// Unused id next to the row an operation visits for `index`.
inline uint64_t free_id(uint64_t index, uint64_t total) { return visited_id(index, total) + 1; }

/// Secondary key of the row with id `id`.
inline uint64_t secondary_key(uint64_t id) { return id * secondary_key_multiplier; }

} // namespace bench_row_order
