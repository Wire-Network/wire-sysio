#include <fc/variant_object.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/account_object.hpp>
#include <sysio/chain/controller.hpp>
#include <sysio/query_engine_plugin/query_source.hpp>

#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <cctype>

namespace sysio::query_engine_plugin {
namespace {
constexpr uint64_t abi_allocation_factor = 8;
constexpr uint64_t row_allocation_factor = 2;
constexpr auto utc_suffix = "Z";

/// Read only an applied HEAD or irreversible state, never speculative pending writes.
void assert_state(const chain::controller& controller) {
   if (!controller.head().is_valid() || controller.get_read_mode() == chain::db_read_mode::SPECULATIVE)
      throw query_error(error_kind::STATE_UNAVAILABLE, "No usable applied chain state");
}

/// Hash the original ABI bytes without allocating or exposing the controller blob.
fc::sha256 abi_hash(const chain::account_metadata_object& metadata) {
   return fc::sha256::hash(metadata.abi.data(), metadata.abi.size());
}
} // namespace

std::vector<table_schema> local_table_source::describe(const ast_query& ast, query_budget& budget) const {
   budget.check();
   assert_state(controller);
   budget.charge_memory(ast.owners.size() * sizeof(table_schema));
   std::vector<table_schema> schemas;
   schemas.reserve(ast.owners.size());
   for (const auto& owner : ast.owners) {
      chain::name account;
      try {
         account = chain::name(owner);
      } catch (const fc::exception&) {
         throw query_error(error_kind::QUERY_SEMANTICS, "Invalid account name");
      }
      const auto* metadata = controller.find_account_metadata(account);
      if (!metadata || !controller.find_account(account))
         throw query_error(error_kind::QUERY_SEMANTICS, "Unknown account");
      budget.assert_limit(metadata->abi.size(), budget.config.max_abi_bytes, option::max_abi_bytes);
      budget.charge_raw(0, metadata->abi.size());
      budget.charge_memory(uint64_t(metadata->abi.size()) * abi_allocation_factor + sizeof(table_schema));
      table_schema result;
      result.owner = account;
      result.abi_hash = abi_hash(*metadata);
      result.abi_sequence = metadata->abi_sequence;
      try {
         result.abi = chain_apis::get_abi(controller, account);
      } catch (const fc::exception&) {
         throw query_error(error_kind::QUERY_SEMANTICS, "Invalid account ABI");
      }
      const auto table = std::find_if(result.abi.tables.begin(), result.abi.tables.end(),
                                      [&](const auto& table) { return table.name == ast.table; });
      if (table == result.abi.tables.end())
         throw query_error(error_kind::QUERY_SEMANTICS, "Unknown ABI table");
      result.table = *table;
      budget.check();
      schemas.emplace_back(std::move(result));
   }
   return schemas;
}

captured_input local_table_source::capture(const typed_plan& plan, query_budget& budget) const {
   const auto started = budget.now();
   budget.check_capture(started);
   assert_state(controller);
   const auto assert_abis = [&](error_kind failure) {
      for (const auto& schema : plan.schemas) {
         budget.check_capture(started);
         const auto* metadata = controller.find_account_metadata(schema->owner);
         if (!metadata || metadata->abi_sequence != schema->abi_sequence || abi_hash(*metadata) != schema->abi_hash)
            throw query_error(failure, "Selected owner's ABI changed");
      }
   };
   assert_abis(error_kind::SCHEMA_CHANGED);
   const auto head = controller.head();
   const auto identity = head.id();
   const auto mode = controller.get_read_mode();
   const auto irreversible = controller.fork_db_has_root() ? controller.fork_db_root().block_num() : 0;
   std::string mode_name(magic_enum::enum_name(mode));
   std::transform(mode_name.begin(), mode_name.end(), mode_name.begin(),
                  [](unsigned char c) { return std::tolower(c); });
   captured_input result;
   constexpr uint64_t state_allocation_bytes = 4096;
   budget.charge_memory(state_allocation_bytes * (plan.schemas.size() + 1));
   fc::variants abis;
   for (const auto& schema : plan.schemas)
      abis.emplace_back(fc::mutable_variant_object()(response_field::owner, schema->owner.to_string())(
         response_field::hash, schema->abi_hash.str()));
   result.state = fc::mutable_variant_object()(response_field::chain_id, controller.get_chain_id().str())(
      response_field::block_id, identity.str())(response_field::block_num, std::to_string(head.block_num()))(
      response_field::block_time, head.block_time().to_iso_string())(response_field::read_mode, mode_name)(
      response_field::last_irreversible_block_num, std::to_string(irreversible))(response_field::abis, std::move(abis))(
      response_field::captured_at, fc::time_point::now().to_iso_string() + utc_suffix)(response_field::synced,
                                                                                       controller.is_synced());
   chain_apis::table_read_budget read_budget;
   read_budget.check = [&] { budget.check_capture(started); };
   read_budget.before_copy = [&](uint64_t rows, uint64_t bytes) { budget.charge_raw(rows, bytes); };
   if (!plan.empty_range) {
      for (const auto& schema : plan.schemas) {
         auto scan = plan.scan;
         scan.code = schema->owner;
         scan.table_id = schema->table.table_id;
         for (;;) {
            auto page = chain_apis::capture_primary_page(controller, scan, read_budget);
            ++result.native_pages;
            budget.charge_memory(page.rows.size() * sizeof(captured_row) * row_allocation_factor);
            for (auto& row : page.rows)
               result.rows.push_back({std::move(row), schema->owner});
            if (!page.more)
               break;
            scan.lower = std::move(page.resume);
         }
      }
   }
   budget.check_capture(started);
   assert_abis(error_kind::STATE_UNAVAILABLE);
   if (controller.head().id() != identity || controller.get_read_mode() != mode)
      throw query_error(error_kind::STATE_UNAVAILABLE, "Chain state changed inside read callback");
   result.capture_us = std::chrono::duration_cast<std::chrono::microseconds>(budget.now() - started).count();
   return result;
}
} // namespace sysio::query_engine_plugin
