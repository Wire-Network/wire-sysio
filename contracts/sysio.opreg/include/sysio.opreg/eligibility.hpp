#pragma once

#include <sysio.opreg/sysio.opreg.hpp>
#include <sysio.opp.common/safe_ops.hpp>

namespace sysio::opreg_eligibility {

/// Collateral changes must never reverse punishment or permanent removal.
inline bool has_terminal_status(opp::types::OperatorStatus status) {
   return status == opp::types::OPERATOR_STATUS_SLASHED || status == opp::types::OPERATOR_STATUS_TERMINATED;
}

/// Sum one account's pending reservations for a collateral pair, saturating external uint64 amounts.
/// There is at most one reservation per collateral bucket; no other account's queue is traversed.
inline uint64_t sum_pending_withdraws(name account, slug_name chain_code, slug_name token_code) {
   opreg::wtdwqueue_t queue(opreg::ACCOUNT);
   auto index = queue.get_index<"byaccount"_n>();
   uint64_t total = 0;
   const auto end = index.upper_bound(account.value);
   for (auto it = index.lower_bound(account.value); it != end; ++it) {
      if (it->chain_code == chain_code && it->token_code == token_code) {
         total = opp::safe::add_sat_u64(total, it->amount);
      }
   }
   return total;
}

/// Find the posted balance for a collateral pair without copying the operator's balance vector.
inline const opreg::balance_entry* find_balance(const opreg::operator_entry& op,
                                                slug_name chain_code, slug_name token_code) {
   for (const auto& balance : op.balances) {
      if (balance.chain_code == chain_code && balance.token_code == token_code) return &balance;
   }
   return nullptr;
}

/// Available collateral is posted balance minus pending withdrawals; terminal rows have none.
inline uint64_t available_inline(const opreg::operator_entry& op, slug_name chain_code, slug_name token_code) {
   if (has_terminal_status(op.status)) return 0;
   const auto* balance = find_balance(op, chain_code, token_code);
   if (!balance) return 0;
   const auto pending = sum_pending_withdraws(op.account, chain_code, token_code);
   return balance->balance > pending ? balance->balance - pending : 0;
}

/// Evaluate the current per-role policy independently of stored ACTIVE/UNKNOWN status.
/// Bootstrap rows are collateral-exempt; an empty policy cannot activate an ordinary operator.
inline bool meets_role_min(const opreg::operator_entry& op, const opreg::op_config& cfg) {
   if (has_terminal_status(op.status)) return false;
   if (op.is_bootstrapped) return true;
   const std::vector<opreg::chain_min_bond>* requirements = nullptr;
   switch (op.type) {
      case opp::types::OPERATOR_TYPE_PRODUCER: requirements = &cfg.req_prod_collat; break;
      case opp::types::OPERATOR_TYPE_BATCH: requirements = &cfg.req_batchop_collat; break;
      case opp::types::OPERATOR_TYPE_UNDERWRITER: requirements = &cfg.req_uw_collat; break;
      default: return false;
   }
   if (requirements->empty()) return false;
   for (const auto& requirement : *requirements) {
      if (available_inline(op, requirement.chain_code, requirement.token_code) < requirement.min_bond) return false;
   }
   return true;
}

/// Batch admission, consensus, publication and rewards must not trust ACTIVE earned under an older policy.
/// Callers load the config once per operation; the sweep reconciles persisted status separately.
inline bool is_active_batch_operator(const opreg::operator_entry& op, const opreg::op_config& cfg) {
   return op.type == opp::types::OPERATOR_TYPE_BATCH && op.status == opp::types::OPERATOR_STATUS_ACTIVE &&
          meets_role_min(op, cfg);
}

/// Read the authoritative policy, treating an absent configuration as empty requirements.
inline opreg::op_config current_config() {
   opreg::opconfig_t table(opreg::ACCOUNT);
   return table.get_or_default(opreg::op_config{});
}

} // namespace sysio::opreg_eligibility
