#pragma once
/**
 * @file table_range.hpp
 * @brief A secondary-index key range and its read page by page, over a caller-supplied page reader, so the relay
 *        reads every row of a non-unique key rather than the one row an exact-match lookup returns.
 */

#include <iterator>
#include <optional>
#include <string>
#include <utility>

#include <fc/variant.hpp>

namespace sysio::batch_operator_detail {

/// A secondary-index key range in the JSON bounds `get_table_rows` takes: from `lower_bound` (inclusive) to
/// `upper_bound` (exclusive; empty reads to the end of the index).
struct index_range {
   std::string lower_bound;
   std::string upper_bound;
};

/// One page of a range read: its rows, and the cursor to resume from while the range has more.
struct table_page {
   fc::variants rows;
   bool         more = false;
   std::string  next_key;
};

/// Every row of `range`, read with `read_page(lower_bound, upper_bound)` (nullopt when that read failed): first from
/// the range's lower bound, then from each page's `next_key`, until a page reports no more rows. Nullopt when a read
/// failed or a page that reports more does not move the cursor.
template <typename ReadPage>
std::optional<fc::variants> read_range(const index_range& range, ReadPage&& read_page) {
   fc::variants rows;
   std::string  lower_bound = range.lower_bound;
   for (;;) {
      std::optional<table_page> page = read_page(lower_bound, range.upper_bound);
      if (!page) return std::nullopt;
      rows.insert(rows.end(), std::make_move_iterator(page->rows.begin()), std::make_move_iterator(page->rows.end()));
      if (!page->more) return rows;
      if (page->next_key.empty() || page->next_key == lower_bound) return std::nullopt;
      lower_bound = std::move(page->next_key);
   }
}

} // namespace sysio::batch_operator_detail
