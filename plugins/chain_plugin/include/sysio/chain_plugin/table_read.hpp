#pragma once

#include <fc/time.hpp>
#include <sysio/chain/abi_def.hpp>

#include <functional>
#include <optional>

namespace sysio::chain {
class controller;
}

namespace sysio::chain_apis {

/// A primary row whose storage is independent of the controller and its undo history.
struct owned_table_row {
   std::vector<char> key; ///< Full primary key, including the scope prefix.
   std::vector<char> value;
   chain::name payer;
};

/// Direction of a primary scan; reverse continuation uses an exclusive upper bound.
enum class scan_direction { forward, reverse };

/// A half-open range in one account/table partition. No controller references escape.
struct primary_scan_request {
   chain::name code;
   uint16_t table_id = 0;
   std::vector<char> lower;
   std::optional<std::vector<char>> upper;
   scan_direction direction = scan_direction::forward;
   uint32_t page_rows = 0;
};

/// Shared across pages. Strict callers provide both callbacks, which throw on exhaustion.
/// Compatibility callers omit them and retain the public API's post-row soft deadline.
struct table_read_budget {
   fc::time_point deadline = fc::time_point::maximum();
   std::function<void()> check;
   std::function<void(uint64_t rows, uint64_t bytes)> before_copy;
   uint64_t rows = 0;
   uint64_t bytes = 0;
};

/// Owned page plus continuation; strict budget exhaustion never returns partial success.
struct primary_scan_page {
   std::vector<owned_table_row> rows;
   std::vector<char> resume;
   bool more = false;
};

/// Existing account/ABI lookup, shared with the public table API. Read queue only.
chain::abi_def get_abi(const chain::controller&, const chain::name&);

/// Capture one page on the chain read queue. All pages of a coherent query must run
/// within the SAME callback; never keep database iterators between callbacks.
primary_scan_page capture_primary_page(const chain::controller&, const primary_scan_request&, table_read_budget&);

/// Exclusive upper bound for all byte strings beginning with prefix; nullopt on overflow.
std::optional<std::vector<char>> primary_prefix_upper(std::vector<char> prefix);

} // namespace sysio::chain_apis
