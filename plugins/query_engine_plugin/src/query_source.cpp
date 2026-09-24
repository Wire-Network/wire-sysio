#include <fc/variant_object.hpp>
#include <sysio/chain/account_object.hpp>
#include <sysio/chain/controller.hpp>
#include <sysio/query_engine_plugin/query_source.hpp>

#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace sysio::query_engine {
namespace {
constexpr uint64_t abi_copy_allocation_factor = 2;
constexpr uint64_t row_allocation_factor = 2;
constexpr auto utc_suffix = "Z";

/// Read only an applied HEAD or irreversible state, never speculative pending writes.
void assert_state(const chain::controller& controller) {
   if (!controller.head().is_valid() || controller.get_read_mode() == chain::db_read_mode::SPECULATIVE)
      throw query_error(error_kind::STATE_UNAVAILABLE, "No usable applied chain state");
}

/// Whether the account's ABI bytes still equal the copy a schema was resolved from. A byte comparison
/// keeps the read callback O(bytes) without hashing inside the chain's read window.
bool same_abi(const chain::account_metadata_object& metadata, const table_schema& schema) {
   return metadata.abi_sequence == schema.abi_sequence && metadata.abi.size() == schema.abi_bytes.size() &&
          (schema.abi_bytes.empty() ||
           std::memcmp(metadata.abi.data(), schema.abi_bytes.data(), schema.abi_bytes.size()) == 0);
}
} // namespace

std::vector<table_schema> local_table_source::capture_abis(const ast_query& ast, query_budget& budget) const {
   // Bounded like a data capture: the bytes copied here hold the read window too.
   const auto started = budget.now();
   budget.check_capture(started);
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
      budget.charge_memory(uint64_t(metadata->abi.size()) * abi_copy_allocation_factor + sizeof(table_schema));
      // Only the bytes leave the callback: hashing, decoding and table selection run on a worker.
      table_schema result;
      result.owner = account;
      result.abi_sequence = metadata->abi_sequence;
      result.abi_bytes.assign(metadata->abi.data(), metadata->abi.data() + metadata->abi.size());
      budget.check_capture(started);
      schemas.emplace_back(std::move(result));
   }
   return schemas;
}

std::vector<table_schema> local_table_source::describe(const ast_query& ast, query_budget& budget) const {
   auto schemas = capture_abis(ast, budget);
   for (auto& schema : schemas)
      resolve_schema(schema, ast.table, budget);
   return schemas;
}

captured_input local_table_source::capture(const typed_plan& plan, query_budget& budget) const {
   const auto started = budget.now();
   budget.check_capture(started);
   assert_state(controller);
   // Nothing can rewrite an ABI while this callback holds the read window, so one comparison at the
   // start covers the whole capture.
   for (const auto& schema : plan.schemas) {
      budget.check_capture(started);
      const auto* metadata = controller.find_account_metadata(schema->owner);
      if (!metadata || !same_abi(*metadata, *schema))
         throw query_error(error_kind::SCHEMA_CHANGED, "Selected owner's ABI changed");
   }
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
   result.capture_us = std::chrono::duration_cast<std::chrono::microseconds>(budget.now() - started).count();
   return result;
}
} // namespace sysio::query_engine
