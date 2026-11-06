#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/account_object.hpp>
#include <sysio/chain/controller.hpp>
#include <sysio/chain/kv_store.hpp>
#include <sysio/chain_plugin/table_read.hpp>

namespace sysio::chain_apis {

chain::abi_def get_abi(const chain::controller& db, const chain::name& account) {
   SYS_ASSERT(db.find_account(account) != nullptr, chain::account_query_exception, "Fail to retrieve account for {}",
              account);
   chain::abi_def abi;
   if (const auto* metadata = db.find_account_metadata(account))
      chain::abi_serializer::to_abi(metadata->abi, abi);
   return abi;
}

std::optional<std::vector<char>> primary_prefix_upper(std::vector<char> prefix) {
   const auto upper = chain::kv_prefix_successor({prefix.data(), prefix.size()});
   if (!upper)
      return std::nullopt;
   return std::vector<char>(upper->begin(), upper->end());
}

primary_scan_page capture_primary_page(const chain::controller& db, const primary_scan_request& request,
                                       table_read_budget& budget) {
   if (budget.check)
      budget.check();
   primary_scan_page page;
   auto cursor = db.kv().open_row_cursor();
   chain::kv_row row;
   const std::string_view lower(request.lower.data(), request.lower.size());
   const auto upper = request.upper
                         ? std::optional<std::string_view>(std::in_place, request.upper->data(), request.upper->size())
                         : std::nullopt;
   const auto in_partition = [&](const chain::kv_row& at) {
      return at.code == request.code && at.table_id == request.table_id;
   };
   const auto continuation = [&](std::string_view key) {
      if (budget.before_copy)
         budget.before_copy(0, key.size());
      budget.bytes += key.size();
      page.resume.assign(key.begin(), key.end());
      page.more = true;
   };
   // A compatibility caller's page ends after the row that reaches the deadline or the byte limit.
   const auto soft_limit_reached = [&] {
      return !budget.check && (fc::time_point::now() >= budget.deadline || budget.bytes >= budget.max_bytes);
   };
   const auto copy_row = [&] {
      if (budget.check)
         budget.check();
      // Charge before copying.
      const uint64_t bytes = owned_table_row_bytes(row.key.size(), row.value.size());
      if (budget.before_copy)
         budget.before_copy(1, bytes);
      owned_table_row owned;
      owned.key.assign(row.key.begin(), row.key.end());
      owned.value.assign(row.value.begin(), row.value.end());
      owned.payer = row.payer;
      page.rows.emplace_back(std::move(owned));
      ++budget.rows;
      budget.bytes += bytes;
   };
   if (request.direction == scan_direction::forward) {
      bool on_row = cursor->seek_lower({request.code, request.table_id, lower}, row);
      const auto in_range = [&] { return on_row && in_partition(row) && (!upper || row.key < *upper); };
      while (in_range()) {
         if (page.rows.size() >= request.page_rows) {
            continuation(row.key);
            break;
         }
         copy_row();
         on_row = cursor->next(row);
         if (soft_limit_reached()) {
            if (in_range())
               continuation(row.key);
            break;
         }
      }
   } else {
      if (upper)
         cursor->seek_lower({request.code, request.table_id, *upper}, row);
      else
         cursor->seek_past_table(request.code, request.table_id, row);
      while (cursor->prev(row) && in_partition(row) && row.key >= lower) {
         if (page.rows.size() >= request.page_rows) {
            if (!page.rows.empty()) {
               const auto& key = page.rows.back().key;
               continuation(std::string_view(key.data(), key.size()));
            }
            break;
         }
         copy_row();
         if (soft_limit_reached()) {
            // Check that the row below is still in range.
            chain::kv_row below;
            if (cursor->clone()->prev(below) && in_partition(below) && below.key >= lower)
               continuation(row.key);
            break;
         }
      }
   }
   if (budget.check)
      budget.check();
   return page;
}

} // namespace sysio::chain_apis
