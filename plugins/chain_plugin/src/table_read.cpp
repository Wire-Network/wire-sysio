#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/account_object.hpp>
#include <sysio/chain/controller.hpp>
#include <sysio/chain/kv_table_objects.hpp>
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
   constexpr unsigned char last_byte = 0xff;
   while (!prefix.empty()) {
      const auto last = static_cast<unsigned char>(prefix.back());
      if (last != last_byte) {
         prefix.back() = static_cast<char>(last + 1);
         return prefix;
      }
      prefix.pop_back();
   }
   return std::nullopt;
}

primary_scan_page capture_primary_page(const chain::controller& db, const primary_scan_request& request,
                                       table_read_budget& budget) {
   if (budget.check)
      budget.check();
   primary_scan_page page;
   const auto& index = db.db().get_index<chain::kv_index, chain::by_code_key>();
   const std::string_view lower(request.lower.data(), request.lower.size());
   const auto upper = request.upper
                         ? std::optional<std::string_view>(std::in_place, request.upper->data(), request.upper->size())
                         : std::nullopt;
   const auto in_partition = [&](auto iterator) {
      return iterator != index.end() && iterator->code == request.code && iterator->table_id == request.table_id;
   };
   const auto continuation = [&](std::string_view key) {
      if (budget.before_copy)
         budget.before_copy(0, key.size());
      budget.bytes += key.size();
      page.resume.assign(key.begin(), key.end());
      page.more = true;
   };
   const auto copy_row = [&](const chain::kv_object& row) {
      if (budget.check)
         budget.check();
      // Two row slots cover vector growth's temporary allocations. Charge before copying.
      const uint64_t bytes = row.key.size() + row.value.size() + 2 * sizeof(owned_table_row);
      if (budget.before_copy)
         budget.before_copy(1, bytes);
      owned_table_row owned;
      owned.key.assign(row.key.data(), row.key.data() + row.key.size());
      owned.value.assign(row.value.data(), row.value.data() + row.value.size());
      owned.payer = row.payer;
      page.rows.emplace_back(std::move(owned));
      ++budget.rows;
      budget.bytes += bytes;
   };
   if (request.direction == scan_direction::forward) {
      auto iterator = index.lower_bound(boost::make_tuple(request.code, request.table_id, lower));
      while (in_partition(iterator) && (!upper || iterator->key_view() < *upper)) {
         if (page.rows.size() >= request.page_rows) {
            continuation(iterator->key_view());
            break;
         }
         copy_row(*iterator++);
         if (!budget.check && fc::time_point::now() >= budget.deadline) {
            if (in_partition(iterator) && (!upper || iterator->key_view() < *upper))
               continuation(iterator->key_view());
            break;
         }
      }
   } else {
      auto iterator = upper ? index.lower_bound(boost::make_tuple(request.code, request.table_id, *upper))
                            : index.upper_bound(boost::make_tuple(request.code, request.table_id));
      const auto begin = index.lower_bound(boost::make_tuple(request.code, request.table_id, std::string_view()));
      while (iterator != begin) {
         --iterator;
         if (!in_partition(iterator) || iterator->key_view() < lower)
            break;
         if (page.rows.size() >= request.page_rows) {
            if (!page.rows.empty()) {
               const auto& key = page.rows.back().key;
               continuation(std::string_view(key.data(), key.size()));
            }
            break;
         }
         copy_row(*iterator);
         if (iterator != begin && !budget.check && fc::time_point::now() >= budget.deadline) {
            continuation(iterator->key_view());
            break;
         }
      }
   }
   if (budget.check)
      budget.check();
   return page;
}

} // namespace sysio::chain_apis
