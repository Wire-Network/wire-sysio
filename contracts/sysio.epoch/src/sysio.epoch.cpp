#include <sysio.epoch/sysio.epoch.hpp>
#include <sysio/opp/attestations/attestations.pb.hpp>
#include <zpp_bits.h>

namespace sysio {

using opp::types::OperatorType;
using opp::types::AttestationType;
using opp::types::OperatorStatus;

// ---------------------------------------------------------------------------
//  setconfig
// ---------------------------------------------------------------------------
void epoch::setconfig(uint32_t epoch_duration_sec,
                      uint32_t operators_per_epoch,
                      uint32_t batch_operator_minimum_active,
                      uint32_t batch_op_groups,
                      uint32_t warmup_epochs,
                      uint32_t cooldown_epochs) {
   require_auth(get_self());

   check(epoch_duration_sec > 0, "epoch_duration_sec must be positive");
   check(operators_per_epoch > 0, "operators_per_epoch must be positive");
   check(batch_op_groups > 0, "batch_op_groups must be positive");
   check(batch_operator_minimum_active == operators_per_epoch * batch_op_groups,
         "batch_operator_minimum_active must equal operators_per_epoch * batch_op_groups");

   epochcfg_t cfg_tbl(get_self(), get_self().value);
   epoch_config cfg;
   if (cfg_tbl.exists()) {
      cfg = cfg_tbl.get();
   }
   cfg.epoch_duration_sec = epoch_duration_sec;
   cfg.operators_per_epoch = operators_per_epoch;
   cfg.batch_operator_minimum_active = batch_operator_minimum_active;
   cfg.batch_op_groups = batch_op_groups;
   cfg.warmup_epochs = warmup_epochs;
   cfg.cooldown_epochs = cooldown_epochs;
   cfg_tbl.set(cfg, get_self());
}

// ---------------------------------------------------------------------------
//  regoperator
// ---------------------------------------------------------------------------
void epoch::regoperator(name account, opp::types::OperatorType type) {
   require_auth(get_self());

   check(type == OperatorType::OPERATOR_TYPE_BATCH || type == OperatorType::OPERATOR_TYPE_UNDERWRITER || type == OperatorType::OPERATOR_TYPE_CHALLENGER,
         "invalid operator type");

   epochstate_t state_tbl(get_self(), get_self().value);
   epoch_state state;
   if (state_tbl.exists()) {
      state = state_tbl.get();
   }

   operators_t ops(get_self(), get_self().value);
   auto it = ops.find(account.value);
   if (it == ops.end()) {
      ops.emplace(get_self(), [&](auto& o) {
         o.account = account;
         o.type = type;
         o.status = OperatorStatus::OPERATOR_STATUS_WARMUP;
         o.registered_epoch = state.current_epoch_index;
         o.assigned_batch_op_group = 255; // unassigned until initgroups
         o.last_elected_epoch = 0;
         o.slash_count = 0;
         o.is_blacklisted = false;
      });
   } else {
      check(!it->is_blacklisted, "operator is blacklisted");
      ops.modify(it, same_payer, [&](auto& o) {
         o.type = type;
         o.status = OperatorStatus::OPERATOR_STATUS_WARMUP;
         o.registered_epoch = state.current_epoch_index;
      });
   }
}

// ---------------------------------------------------------------------------
//  activateop
// ---------------------------------------------------------------------------
void epoch::activateop(name account) {
   require_auth(get_self());

   operators_t ops(get_self(), get_self().value);
   auto it = ops.find(account.value);
   check(it != ops.end(), "operator not found");
   check(it->status == OperatorStatus::OPERATOR_STATUS_WARMUP, "operator is not in warmup");

   ops.modify(it, same_payer, [&](auto& o) {
      o.status = OperatorStatus::OPERATOR_STATUS_ACTIVE;
   });
}

// ---------------------------------------------------------------------------
//  unregoper
// ---------------------------------------------------------------------------
void epoch::unregoper(name account) {
   require_auth(account);

   operators_t ops(get_self(), get_self().value);
   auto it = ops.find(account.value);
   check(it != ops.end(), "operator not found");
   check(it->status == OperatorStatus::OPERATOR_STATUS_ACTIVE, "operator must be active to deregister");

   ops.modify(it, same_payer, [&](auto& o) {
      o.status = OperatorStatus::OPERATOR_STATUS_COOLDOWN;
   });
}

// ---------------------------------------------------------------------------
//  advance
// ---------------------------------------------------------------------------
void epoch::advance() {
   epochcfg_t cfg_tbl(get_self(), get_self().value);
   check(cfg_tbl.exists(), "epoch config not initialized");
   auto cfg = cfg_tbl.get();

   epochstate_t state_tbl(get_self(), get_self().value);
   epoch_state state;
   if (state_tbl.exists()) {
      state = state_tbl.get();
   }

   check(!state.is_paused, "epoch advancement is paused");

   auto now = current_time_point();
   // If the epoch duration has not elapsed, return silently (idempotent no-op)
   if (now < state.next_epoch_start) return;

   state.current_epoch_index++;
   state.current_batch_op_group = state.current_epoch_index % cfg.batch_op_groups;

   // On first advance, next_epoch_start is default (epoch 0). Use current
   // block time as the epoch start instead of the uninitialized value.
   state.current_epoch_start =
      (state.next_epoch_start.sec_since_epoch() == 0) ? now : state.next_epoch_start;
   state.next_epoch_start = state.current_epoch_start +
      microseconds(static_cast<int64_t>(cfg.epoch_duration_sec) * 1'000'000);

   // Update last_elected_epoch for operators in the active group
   if (state.current_batch_op_group < state.batch_op_groups.size()) {
      operators_t ops(get_self(), get_self().value);
      for (const auto& op_name : state.batch_op_groups[state.current_batch_op_group]) {
         auto it = ops.find(op_name.value);
         if (it != ops.end()) {
            ops.modify(it, same_payer, [&](auto& o) {
               o.last_elected_epoch = state.current_epoch_index;
            });
         }
      }
   }

   state_tbl.set(state, get_self());

   // Queue BATCH_OPERATOR_NEXT_GROUP attestation for each outpost.
   // This tells outposts which batch operators will be active in the next epoch.
   uint8_t next_group_idx = (state.current_epoch_index + 1) % cfg.batch_op_groups;
   if (next_group_idx < state.batch_op_groups.size()) {
      auto& next_group = state.batch_op_groups[next_group_idx];

      // Build the protobuf attestation
      opp::attestations::BatchOperatorNextGroup attest;
      attest.group_index = zpp::bits::vuint32_t{next_group_idx};
      attest.next_epoch_index = zpp::bits::vuint32_t{state.current_epoch_index + 1};
      for (auto& op_name : next_group) {
         opp::types::ChainAddress addr;
         addr.kind = opp::types::CHAIN_KIND_WIRE;
         auto name_str = op_name.to_string();
         addr.address.assign(name_str.begin(), name_str.end());
         attest.operators.push_back(std::move(addr));
      }

      // Serialize with zpp_bits (protobuf wire format)
      auto [packed, out] = zpp::bits::data_out<char>();
      (void)out(attest);

      // Queue to sysio.msgch for each registered outpost
      outposts_t outposts_tbl(get_self(), get_self().value);
      for (auto it = outposts_tbl.begin(); it != outposts_tbl.end(); ++it) {
         action(
            permission_level{"sysio.epoch"_n, "owner"_n},
            MSGCH_ACCOUNT,
            "queueout"_n,
            std::make_tuple(
               it->id,
               opp::types::ATTESTATION_TYPE_BATCH_OPERATOR_NEXT_GROUP,
               packed
            )
         ).send();
      }
   }

   // Notify sysio.msgch to process pending consensus results
   require_recipient(MSGCH_ACCOUNT);
}

// ---------------------------------------------------------------------------
//  initgroups
// ---------------------------------------------------------------------------
void epoch::initgroups() {
   require_auth(get_self());

   epochcfg_t cfg_tbl(get_self(), get_self().value);
   check(cfg_tbl.exists(), "epoch config not initialized");
   auto cfg = cfg_tbl.get();

   operators_t ops(get_self(), get_self().value);

   // Collect all ACTIVE batch operators ordered by registration epoch
   std::vector<name> active_batch;
   auto status_idx = ops.get_index<"bystatus"_n>();
   for (auto it = status_idx.lower_bound(OperatorStatus::OPERATOR_STATUS_ACTIVE);
        it != status_idx.end() && it->status == OperatorStatus::OPERATOR_STATUS_ACTIVE; ++it) {
      if (it->type == OperatorType::OPERATOR_TYPE_BATCH && !it->is_blacklisted) {
         active_batch.push_back(it->account);
      }
   }

   check(active_batch.size() >= cfg.batch_operator_minimum_active,
         "not enough active batch operators for group assignment");

   // Sort by registration epoch (stable, deterministic)
   std::sort(active_batch.begin(), active_batch.end(), [&](const name& a, const name& b) {
      auto ia = ops.find(a.value);
      auto ib = ops.find(b.value);
      return ia->registered_epoch < ib->registered_epoch;
   });

   // Trim to exactly batch_operator_minimum_active
   active_batch.resize(cfg.batch_operator_minimum_active);

   // Even/odd interleave shuffle
   std::vector<name> even_list, odd_list;
   for (size_t i = 0; i < active_batch.size(); ++i) {
      if (i % 2 == 0) {
         even_list.push_back(active_batch[i]);
      } else {
         odd_list.push_back(active_batch[i]);
      }
   }
   std::vector<name> shuffled;
   shuffled.insert(shuffled.end(), even_list.begin(), even_list.end());
   shuffled.insert(shuffled.end(), odd_list.begin(), odd_list.end());

   // Divide into groups
   std::vector<std::vector<name>> new_groups;
   for (uint32_t g = 0; g < cfg.batch_op_groups; ++g) {
      std::vector<name> group;
      uint32_t start = g * cfg.operators_per_epoch;
      uint32_t end = start + cfg.operators_per_epoch;
      for (uint32_t i = start; i < end && i < shuffled.size(); ++i) {
         group.push_back(shuffled[i]);
      }
      new_groups.push_back(group);
   }

   // Update operator assigned_batch_op_group
   for (uint8_t g = 0; g < new_groups.size(); ++g) {
      for (const auto& op_name : new_groups[g]) {
         auto it = ops.find(op_name.value);
         if (it != ops.end()) {
            ops.modify(it, same_payer, [&](auto& o) {
               o.assigned_batch_op_group = g;
            });
         }
      }
   }

   // Store groups in epoch state
   epochstate_t state_tbl(get_self(), get_self().value);
   epoch_state state;
   if (state_tbl.exists()) {
      state = state_tbl.get();
   }
   state.batch_op_groups = new_groups;
   state_tbl.set(state, get_self());
}

// ---------------------------------------------------------------------------
//  replaceop
// ---------------------------------------------------------------------------
void epoch::replaceop(name old_op, name new_op) {
   require_auth(get_self());

   operators_t ops(get_self(), get_self().value);

   auto old_it = ops.find(old_op.value);
   check(old_it != ops.end(), "old operator not found");

   auto new_it = ops.find(new_op.value);
   check(new_it != ops.end(), "new operator not found");
   check(new_it->type == OperatorType::OPERATOR_TYPE_BATCH, "replacement must be a batch operator");
   check(new_it->status == OperatorStatus::OPERATOR_STATUS_ACTIVE, "replacement must be active");
   check(!new_it->is_blacklisted, "replacement is blacklisted");

   uint8_t group = old_it->assigned_batch_op_group;
   check(group < 3, "old operator has no valid group assignment");

   // Update assigned_batch_op_group on both operators
   ops.modify(new_it, same_payer, [&](auto& o) {
      o.assigned_batch_op_group = group;
   });
   ops.modify(old_it, same_payer, [&](auto& o) {
      o.assigned_batch_op_group = 255; // unassigned
   });

   // Replace in epoch state groups
   epochstate_t state_tbl(get_self(), get_self().value);
   check(state_tbl.exists(), "epoch state not initialized");
   auto state = state_tbl.get();

   check(group < state.batch_op_groups.size(), "group index out of range");
   bool found = false;
   for (auto& member : state.batch_op_groups[group]) {
      if (member == old_op) {
         member = new_op;
         found = true;
         break;
      }
   }
   check(found, "old operator not found in group");

   state_tbl.set(state, get_self());
}

// ---------------------------------------------------------------------------
//  regoutpost
// ---------------------------------------------------------------------------
void epoch::regoutpost(fc::crypto::chain_kind_t chain_kind, uint32_t chain_id) {
   require_auth(get_self());

   outposts_t outposts(get_self(), get_self().value);

   // Check for duplicates via secondary index
   auto chain_idx = outposts.get_index<"bychain"_n>();
   uint64_t composite = (static_cast<uint64_t>(chain_kind) << 32) | chain_id;
   auto it = chain_idx.find(composite);
   check(it == chain_idx.end(), "outpost already registered");

   outposts.emplace(get_self(), [&](auto& o) {
      o.id = outposts.available_primary_key();
      o.chain_kind = chain_kind;
      o.chain_id = chain_id;
      o.last_inbound_epoch = 0;
      o.last_outbound_epoch = 0;
   });
}

// ---------------------------------------------------------------------------
//  pause / unpause
// ---------------------------------------------------------------------------
void epoch::pause() {
   require_auth(CHALG_ACCOUNT);

   epochstate_t state_tbl(get_self(), get_self().value);
   epoch_state state;
   if (state_tbl.exists()) {
      state = state_tbl.get();
   }
   state.is_paused = true;
   state_tbl.set(state, get_self());
}

void epoch::unpause() {
   require_auth(CHALG_ACCOUNT);

   epochstate_t state_tbl(get_self(), get_self().value);
   check(state_tbl.exists(), "epoch state not initialized");
   auto state = state_tbl.get();
   state.is_paused = false;
   state_tbl.set(state, get_self());
}

} // namespace sysio
