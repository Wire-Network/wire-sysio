#include <sysio/producer_plugin/subjective_exempt_db.hpp>

#include <sysio/chain/config.hpp>
#include <sysio/chain/controller.hpp>
#include <sysio/chain/kv_store.hpp>
#include <sysio/chain/subjective_billing.hpp>
#include <sysio/opp/types/types.pb.h>
#include <sysio.opreg/opreg_constants.hpp>
#include <sysio.system/subjective_exempt_constants.hpp>

#include <fc/exception/exception.hpp>
#include <fc/log/logger.hpp>

#include <magic_enum/magic_enum.hpp>

#include <array>
#include <memory>
#include <string_view>

namespace sysio {

using namespace sysio::chain;

namespace {

namespace exempt_list = sysiosystem::subjective_exemption;

namespace opreg = sysio::opreg_names;

/// Operator types whose accounts are exempt while they have exempt_status.
constexpr std::array exempt_types = {opp::types::OPERATOR_TYPE_BATCH, opp::types::OPERATOR_TYPE_UNDERWRITER};

/// The status of an operator that is bootstrapped or holds its role's collateral minimum. Registering alone gives
/// OPERATOR_STATUS_UNKNOWN and costs nothing, so it exempts nobody.
constexpr opp::types::OperatorStatus exempt_status = opp::types::OPERATOR_STATUS_ACTIVE;

/// Calls `f(account)` for each sysio.opreg operator whose key in `index` is `value`. Both indexes key a uint64 and
/// the roster is keyed by account, so an entry is [value:8B BE] -> [account:8B BE].
template <typename F>
void for_each_operator(const kv_store& store, std::string_view index, uint64_t value, F&& f) {
   const account_name code{opreg::account};
   const uint16_t     table_id =
      compute_sec_table_id(name{opreg::table_operators}.to_uint64_t(), name{index}.to_uint64_t());
   std::array<char, kv_pri_key_size> sec_key_buf;
   kv_encode_be64(sec_key_buf.data(), value);
   const std::string_view sec_key{sec_key_buf.data(), sec_key_buf.size()};

   const std::unique_ptr<kv_index_cursor> cursor = store.open_index_cursor();
   kv_index_entry                         entry;
   for (bool more = cursor->seek_lower(kv_position{code, table_id, sec_key, {}}, entry);
        more && entry.code == code && entry.table_id == table_id && entry.sec_key == sec_key;
        more = cursor->next(entry)) {
      if (entry.pri_key.size() == kv_pri_key_size)
         f(account_name{kv_decode_be64(entry.pri_key.data())});
   }
}

/// Calls `f(account)` for each account of the sysio.system list, whose rows are keyed [account:8B BE].
template <typename F>
void for_each_listed_account(const kv_store& store, F&& f) {
   const uint16_t table_id = compute_table_id(name{exempt_list::table_exempt}.to_uint64_t());

   const std::unique_ptr<kv_row_cursor> cursor = store.open_row_cursor();
   kv_row                               row;
   // An empty key sorts before every key of the table.
   for (bool more = cursor->seek_lower(kv_position{config::system_account_name, table_id, {}, {}}, row);
        more && row.code == config::system_account_name && row.table_id == table_id; more = cursor->next(row)) {
      if (row.key.size() == kv_pri_key_size)
         f(account_name{kv_decode_be64(row.key.data())});
   }
}

} // anonymous namespace

std::unordered_set<account_name> subjective_exempt_db::load(const kv_store& store) {
   std::unordered_set<account_name> active_operators;
   for_each_operator(store, opreg::index_bystatus, magic_enum::enum_integer(exempt_status),
                     [&](account_name account) { active_operators.insert(account); });

   std::unordered_set<account_name> accounts;
   for (const opp::types::OperatorType type : exempt_types) {
      for_each_operator(store, opreg::index_bytype, magic_enum::enum_integer(type), [&](account_name account) {
         if (active_operators.contains(account)) accounts.insert(account);
      });
   }
   for_each_listed_account(store, [&](account_name account) { accounts.insert(account); });
   return accounts;
}

void subjective_exempt_db::on_irreversible_block(const signed_block_ptr& lib, controller& chain) {
   if (_loaded && lib->block_num() % refresh_interval != 0)
      return;

   try {
      std::unordered_set<account_name> accounts = load(chain.kv());
      subjective_billing&              billing  = chain.get_mutable_subjective_billing();
      if (accounts != billing.get_chain_exempt_accounts()) {
         ilog("Chain state exempts {} accounts from subjective billing", accounts.size());
         billing.set_chain_exempt_accounts(std::move(accounts));
      }
      _loaded = true;
   }
   FC_LOG_AND_DROP()
}

} // namespace sysio
