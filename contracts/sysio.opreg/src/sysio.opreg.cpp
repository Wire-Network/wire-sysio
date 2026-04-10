#include <sysio.opreg/sysio.opreg.hpp>
#include <sysio.epoch/sysio.epoch.hpp>
#include <sysio.authex/sysio.authex.hpp>
#include <sysio/opp/attestations/attestations.pb.hpp>
#include <zpp_bits.h>

namespace sysio {

using opp::types::OperatorType;
using opp::types::OperatorStatus;
using opp::types::ChainKind;

// ---------------------------------------------------------------------------
//  Read-only mirror of sysio.authex::links table for cross-contract reads.
//  Must match the on-chain layout exactly (same fields, types, indices).
// ---------------------------------------------------------------------------
namespace authex_readonly {

struct links_row {
   uint64_t                 key;
   name                     username;
   fc::crypto::chain_kind_t chain_kind;
   public_key               pub_key;

   uint64_t  primary_key()   const { return key; }
   uint128_t by_namechain()  const { return to_namechain_key(username, chain_kind); }
   uint64_t  by_name()       const { return username.value; }
   uint64_t  by_chain()      const { return static_cast<uint64_t>(chain_kind); }
};

using links_t = multi_index<"links"_n, links_row,
   indexed_by<"bynamechain"_n, const_mem_fun<links_row, uint128_t, &links_row::by_namechain>>,
   indexed_by<"byname"_n,      const_mem_fun<links_row, uint64_t,  &links_row::by_name>>,
   indexed_by<"bychain"_n,     const_mem_fun<links_row, uint64_t,  &links_row::by_chain>>
>;

} // namespace authex_readonly
using opp::types::AttestationType;

namespace {

uint64_t current_time_ms() {
   return static_cast<uint64_t>(current_time_point().sec_since_epoch()) * 1000;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
//  setconfig
// ---------------------------------------------------------------------------
void opreg::setconfig(uint32_t max_available_producers,
                      uint32_t max_available_batch_ops,
                      uint32_t max_available_underwriters,
                      uint64_t terminate_prune_delay_ms) {
   require_auth(get_self());

   check(max_available_producers > 0, "max_available_producers must be positive");
   check(max_available_batch_ops > 0, "max_available_batch_ops must be positive");
   check(max_available_underwriters > 0, "max_available_underwriters must be positive");
   check(terminate_prune_delay_ms > 0, "terminate_prune_delay_ms must be positive");

   opconfig_t cfg_tbl(get_self(), get_self().value);
   op_config cfg;
   if (cfg_tbl.exists()) {
      cfg = cfg_tbl.get();
   }
   cfg.max_available_producers = max_available_producers;
   cfg.max_available_batch_ops = max_available_batch_ops;
   cfg.max_available_underwriters = max_available_underwriters;
   cfg.terminate_prune_delay_ms = terminate_prune_delay_ms;
   cfg_tbl.set(cfg, get_self());
}

// ---------------------------------------------------------------------------
//  regoperator
// ---------------------------------------------------------------------------
void opreg::regoperator(name account,
                        opp::types::OperatorType type,
                        bool is_bootstrapped) {
   // Privileged sysio.opreg can register any operator.
   // Otherwise the account must authorize its own registration.
   if (!has_auth(get_self())) {
      require_auth(account);
   }

   // Only privileged callers can set is_bootstrapped=true
   if (is_bootstrapped) {
      require_auth(get_self());
   }

   // Validate type
   check(type == OperatorType::OPERATOR_TYPE_PRODUCER ||
         type == OperatorType::OPERATOR_TYPE_BATCH ||
         type == OperatorType::OPERATOR_TYPE_UNDERWRITER ||
         type == OperatorType::OPERATOR_TYPE_CHALLENGER,
         "invalid operator type");

   // Underwriters can NEVER be bootstrapped
   check(!(type == OperatorType::OPERATOR_TYPE_UNDERWRITER && is_bootstrapped),
         "underwriter type cannot be bootstrapped");

   // Check not already registered (non-pruned)
   operators_t ops(get_self(), get_self().value);
   auto it = ops.find(account.value);
   if (it != ops.end()) {
      check(it->status == OperatorStatus::OPERATOR_STATUS_TERMINATED,
            "operator already registered");
      // If terminated, erase old row to allow re-registration
      ops.erase(it);
   }

   // Verify authex links exist for all active outpost chains.
   // Skip when: bootstrapped OR privileged caller (sysio.opreg registering on behalf)
   if (!is_bootstrapped && !has_auth(get_self())) {
      epoch::outposts_t outposts(EPOCH_ACCOUNT, EPOCH_ACCOUNT.value);
      authex_readonly::links_t links(AUTHEX_ACCOUNT, AUTHEX_ACCOUNT.value);
      auto namechain_idx = links.get_index<"bynamechain"_n>();

      for (auto op_it = outposts.begin(); op_it != outposts.end(); ++op_it) {
         // authex uses fc::crypto::chain_kind_t which has identical numeric values
         // to opp::types::ChainKind (ethereum=2, solana=3, sui=4)
         auto chain = static_cast<fc::crypto::chain_kind_t>(op_it->chain_kind);
         uint128_t composite_key = to_namechain_key(account, chain);
         auto link_it = namechain_idx.find(composite_key);
         check(link_it != namechain_idx.end(),
               "missing authex link for outpost chain");
      }
   }

   auto now = current_time_ms();
   ops.emplace(get_self(), [&](auto& o) {
      o.account        = account;
      o.type           = type;
      o.is_bootstrapped = is_bootstrapped;
      o.registered_at  = now;
      if (is_bootstrapped) {
         o.status      = OperatorStatus::OPERATOR_STATUS_ACTIVE; // AVAILABLE
         o.available_at = now;
      } else {
         o.status      = OperatorStatus::OPERATOR_STATUS_UNKNOWN; // PENDING
      }
   });
}

// ---------------------------------------------------------------------------
//  stake — handles both deposits (positive) and withdrawals (negative)
// ---------------------------------------------------------------------------
void opreg::stake(name account,
                  opp::types::ChainAddress chain_addr,
                  opp::types::TokenAmount amount) {
   operators_t ops(get_self(), get_self().value);
   auto it = ops.find(account.value);
   check(it != ops.end(), "operator not found");
   check(it->status != OperatorStatus::OPERATOR_STATUS_SLASHED,
         "slashed operators cannot stake");

   bool is_deposit = static_cast<int64_t>(amount.amount) > 0;
   bool is_wire = (chain_addr.kind == ChainKind::CHAIN_KIND_WIRE);

   // For SYS staking: direct on-chain transfer (does NOT go through OPP)
   if (is_wire) {
      require_auth(account);
      if (is_deposit) {
         action(
            permission_level{account, "active"_n},
            TOKEN_ACCOUNT, "transfer"_n,
            std::make_tuple(account, get_self(),
               asset(static_cast<int64_t>(amount.amount), CORE_SYM),
               std::string("stake"))
         ).send();
      } else {
         action(
            permission_level{get_self(), "active"_n},
            TOKEN_ACCOUNT, "transfer"_n,
            std::make_tuple(get_self(), account,
               asset(-static_cast<int64_t>(amount.amount), CORE_SYM),
               std::string("unstake"))
         ).send();
      }
   }

   // For cross-chain withdrawals: queue OPERATOR_ACTION(WITHDRAW) to outpost
   if (!is_wire && !is_deposit) {
      require_auth(account);
      opp::attestations::OperatorAction oa;
      oa.action_type = opp::attestations::OperatorAction::ACTION_TYPE_WITHDRAW;
      oa.actor = chain_addr;
      opp::types::WireAccount wa;
      wa.name = account.to_string();
      oa.wire_account = wa;
      oa.type = it->type;
      opp::types::TokenAmount pos_amount = amount;
      pos_amount.amount = zpp::bits::vint64_t{-static_cast<int64_t>(amount.amount)};
      oa.amount = pos_amount;

      auto [encoded, out] = zpp::bits::data_out<char>();
      (void)out(oa);

      // Find outpost for this chain
      epoch::outposts_t outposts(EPOCH_ACCOUNT, EPOCH_ACCOUNT.value);
      for (auto op_it = outposts.begin(); op_it != outposts.end(); ++op_it) {
         if (static_cast<int>(op_it->chain_kind) == static_cast<int>(chain_addr.kind)) {
            action(
               permission_level{get_self(), "active"_n},
               MSGCH_ACCOUNT, "queueout"_n,
               std::make_tuple(op_it->id,
                  AttestationType::ATTESTATION_TYPE_OPERATOR_ACTION, encoded)
            ).send();
            break;
         }
      }
   }

   // Append stake entry
   auto now = current_time_ms();
   ops.modify(it, same_payer, [&](auto& o) {
      o.stakes.push_back(stake_entry{chain_addr, amount, now});
   });

   // Re-read after modification
   it = ops.find(account.value);

   // Compute aggregate stakes and check eligibility
   opconfig_t cfg_tbl(get_self(), get_self().value);
   if (!cfg_tbl.exists()) return;
   auto cfg = cfg_tbl.get();

   // Get required stakes for this operator type
   const std::vector<stake_requirement>* reqs = nullptr;
   uint32_t max_available = 0;
   switch (it->type) {
      case OperatorType::OPERATOR_TYPE_PRODUCER:
         reqs = &cfg.req_prod_stakes;
         max_available = cfg.max_available_producers;
         break;
      case OperatorType::OPERATOR_TYPE_BATCH:
         reqs = &cfg.req_batchop_stakes;
         max_available = cfg.max_available_batch_ops;
         break;
      case OperatorType::OPERATOR_TYPE_UNDERWRITER:
         reqs = &cfg.req_uw_stakes;
         max_available = cfg.max_available_underwriters;
         break;
      default:
         return;
   }

   // Compute aggregate per chain_kind+token_kind
   // Compare against requirements
   bool was_eligible = (it->status == OperatorStatus::OPERATOR_STATUS_ACTIVE); // AVAILABLE
   bool is_eligible = true;

   if (reqs && !reqs->empty()) {
      for (const auto& req : *reqs) {
         int64_t aggregate = 0;
         for (const auto& s : it->stakes) {
            if (static_cast<int>(s.chain_addr.kind) == static_cast<int>(req.chain_addr.kind) &&
                static_cast<int>(s.amount.kind) == static_cast<int>(req.min_amount.kind)) {
               aggregate += static_cast<int64_t>(s.amount.amount);
            }
         }
         if (aggregate < static_cast<int64_t>(req.min_amount.amount)) {
            is_eligible = false;
            break;
         }
      }
   } else {
      // No requirements configured — not eligible unless bootstrapped
      is_eligible = it->is_bootstrapped;
   }

   // Check if ALL stakes net to zero → auto-terminate
   bool all_zero = true;
   if (!it->stakes.empty()) {
      // Group by chain_kind+token_kind and sum
      std::vector<std::pair<int, int64_t>> sums; // (chain_kind*1000+token_kind, sum)
      for (const auto& s : it->stakes) {
         int key = static_cast<int>(s.chain_addr.kind) * 1000 + static_cast<int>(s.amount.kind);
         bool found = false;
         for (auto& p : sums) {
            if (p.first == key) {
               p.second += static_cast<int64_t>(s.amount.amount);
               found = true;
               break;
            }
         }
         if (!found) sums.push_back({key, static_cast<int64_t>(s.amount.amount)});
      }
      for (const auto& p : sums) {
         if (p.second != 0) { all_zero = false; break; }
      }
   }

   if (all_zero && !it->stakes.empty()) {
      ops.modify(it, same_payer, [&](auto& o) {
         o.status = OperatorStatus::OPERATOR_STATUS_TERMINATED;
         o.terminated_at = now;
      });
      // Queue ROSTER_UPDATE(TERMINATED) to all outposts
      // TODO: implement queue_roster_update helper
      return;
   }

   // Dispatch type-specific processing if eligibility changed
   if (was_eligible != is_eligible) {
      name action_name;
      switch (it->type) {
         case OperatorType::OPERATOR_TYPE_PRODUCER:
            action_name = "processprod"_n; break;
         case OperatorType::OPERATOR_TYPE_BATCH:
            action_name = "processbatch"_n; break;
         case OperatorType::OPERATOR_TYPE_UNDERWRITER:
            action_name = "processuw"_n; break;
         default: return;
      }
      action(
         permission_level{get_self(), "active"_n},
         get_self(), action_name,
         std::make_tuple(account, was_eligible, is_eligible)
      ).send();
   }

   // For cross-chain deposits: send STAKE_RESULT confirmation back to outpost
   if (!is_wire && is_deposit) {
      // TODO: implement send_stake_result helper
   }
}

// ---------------------------------------------------------------------------
//  processprod / processbatch / processuw
// ---------------------------------------------------------------------------
void opreg::processprod(name account, bool was_eligible, bool is_eligible) {
   require_auth(get_self());
   operators_t ops(get_self(), get_self().value);
   auto it = ops.find(account.value);
   check(it != ops.end(), "operator not found");

   auto now = current_time_ms();

   if (!was_eligible && is_eligible) {
      ops.modify(it, same_payer, [&](auto& o) {
         o.status = OperatorStatus::OPERATOR_STATUS_ACTIVE; // AVAILABLE
         o.available_at = now;
      });
      // For producers: notify system contract
      require_recipient(SYSTEM_ACCOUNT);
      // TODO: queue ROSTER_UPDATE(AVAILABLE) to all outposts
   } else if (was_eligible && !is_eligible) {
      ops.modify(it, same_payer, [&](auto& o) {
         o.status = OperatorStatus::OPERATOR_STATUS_UNKNOWN; // PENDING
      });
      // TODO: queue ROSTER_UPDATE(PENDING) to all outposts
   }
}

void opreg::processbatch(name account, bool was_eligible, bool is_eligible) {
   require_auth(get_self());
   operators_t ops(get_self(), get_self().value);
   auto it = ops.find(account.value);
   check(it != ops.end(), "operator not found");

   auto now = current_time_ms();

   if (!was_eligible && is_eligible) {
      ops.modify(it, same_payer, [&](auto& o) {
         o.status = OperatorStatus::OPERATOR_STATUS_ACTIVE; // AVAILABLE
         o.available_at = now;
      });
      // TODO: queue ROSTER_UPDATE(AVAILABLE) to all outposts
   } else if (was_eligible && !is_eligible) {
      ops.modify(it, same_payer, [&](auto& o) {
         o.status = OperatorStatus::OPERATOR_STATUS_UNKNOWN; // PENDING
      });
      // TODO: queue ROSTER_UPDATE(PENDING) to all outposts
   }
}

void opreg::processuw(name account, bool was_eligible, bool is_eligible) {
   require_auth(get_self());
   operators_t ops(get_self(), get_self().value);
   auto it = ops.find(account.value);
   check(it != ops.end(), "operator not found");

   auto now = current_time_ms();

   if (!was_eligible && is_eligible) {
      ops.modify(it, same_payer, [&](auto& o) {
         o.status = OperatorStatus::OPERATOR_STATUS_ACTIVE; // AVAILABLE
         o.available_at = now;
      });
      // TODO: queue ROSTER_UPDATE(AVAILABLE) to all outposts
   } else if (was_eligible && !is_eligible) {
      ops.modify(it, same_payer, [&](auto& o) {
         o.status = OperatorStatus::OPERATOR_STATUS_UNKNOWN; // PENDING
      });
      // TODO: queue ROSTER_UPDATE(PENDING) to all outposts
   }
}

// ---------------------------------------------------------------------------
//  slash
// ---------------------------------------------------------------------------
void opreg::slash(name account, std::string reason) {
   require_auth(CHALG_ACCOUNT);

   operators_t ops(get_self(), get_self().value);
   auto it = ops.find(account.value);
   check(it != ops.end(), "operator not found");
   check(it->status != OperatorStatus::OPERATOR_STATUS_SLASHED,
         "operator already slashed");

   auto now = current_time_ms();
   ops.modify(it, same_payer, [&](auto& o) {
      o.status = OperatorStatus::OPERATOR_STATUS_SLASHED;
      o.slashed_at = now;
   });

   // Queue SLASH_OPERATOR to all outposts
   epoch::outposts_t outposts(EPOCH_ACCOUNT, EPOCH_ACCOUNT.value);
   for (auto op_it = outposts.begin(); op_it != outposts.end(); ++op_it) {
      opp::attestations::RosterUpdate ru;
      opp::types::ChainAddress addr;
      addr.kind = opp::types::ChainKind::CHAIN_KIND_WIRE;
      auto name_str = account.to_string();
      addr.address.assign(name_str.begin(), name_str.end());
      ru.operator_ = addr;
      ru.type = it->type;
      ru.new_status = OperatorStatus::OPERATOR_STATUS_SLASHED;
      ru.reason = reason;

      auto [encoded, out] = zpp::bits::data_out<char>();
      (void)out(ru);

      action(
         permission_level{get_self(), "active"_n},
         MSGCH_ACCOUNT, "queueout"_n,
         std::make_tuple(op_it->id,
            AttestationType::ATTESTATION_TYPE_SLASH_OPERATOR, encoded)
      ).send();
   }
}

// ---------------------------------------------------------------------------
//  prune — remove terminated operator rows past the delay
// ---------------------------------------------------------------------------
void opreg::prune() {
   opconfig_t cfg_tbl(get_self(), get_self().value);
   check(cfg_tbl.exists(), "opconfig not initialized");
   auto cfg = cfg_tbl.get();

   auto now = current_time_ms();
   operators_t ops(get_self(), get_self().value);
   auto status_idx = ops.get_index<"bystatus"_n>();

   uint32_t removed = 0;
   for (auto it = status_idx.lower_bound(
           static_cast<uint64_t>(OperatorStatus::OPERATOR_STATUS_TERMINATED));
        it != status_idx.end() &&
        it->status == OperatorStatus::OPERATOR_STATUS_TERMINATED;) {
      if (it->terminated_at > 0 && now - it->terminated_at >= cfg.terminate_prune_delay_ms) {
         it = status_idx.erase(it);
         if (++removed >= 20) break; // Bound CPU
      } else {
         ++it;
      }
   }
}

} // namespace sysio
