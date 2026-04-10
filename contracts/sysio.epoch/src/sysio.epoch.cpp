#include <sysio.epoch/sysio.epoch.hpp>
#include <sysio.opreg/sysio.opreg.hpp>
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
                      uint32_t attestation_retention_epoch_count) {
   require_auth(get_self());

   check(epoch_duration_sec > 0, "epoch_duration_sec must be positive");
   check(operators_per_epoch > 0, "operators_per_epoch must be positive");
   check(batch_op_groups > 0, "batch_op_groups must be positive");
   check(batch_operator_minimum_active == operators_per_epoch * batch_op_groups,
         "batch_operator_minimum_active must equal operators_per_epoch * batch_op_groups");
   check(attestation_retention_epoch_count > 0,
         "attestation_retention_epoch_count must be positive");

   epochcfg_t cfg_tbl(get_self(), get_self().value);
   epoch_config cfg;
   if (cfg_tbl.exists()) {
      cfg = cfg_tbl.get();
   }
   cfg.epoch_duration_sec = epoch_duration_sec;
   cfg.operators_per_epoch = operators_per_epoch;
   cfg.batch_operator_minimum_active = batch_operator_minimum_active;
   cfg.batch_op_groups = batch_op_groups;
   cfg.attestation_retention_epoch_count = attestation_retention_epoch_count;
   cfg_tbl.set(cfg, get_self());
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
   if (now < state.next_epoch_start) return;

   state.current_epoch_index++;
   state.current_batch_op_group = state.current_epoch_index % cfg.batch_op_groups;

   state.current_epoch_start =
      (state.next_epoch_start.sec_since_epoch() == 0) ? now : state.next_epoch_start;
   state.next_epoch_start = state.current_epoch_start +
      microseconds(static_cast<int64_t>(cfg.epoch_duration_sec) * 1'000'000);

   // Note: last_elected_epoch tracking is epoch-internal state.
   // No operator table writes needed — group membership is in epoch_state.batch_op_groups.

   state_tbl.set(state, get_self());

   // Queue BATCH_OPERATOR_NEXT_GROUP attestation for each outpost.
   uint8_t next_group_idx = (state.current_epoch_index + 1) % cfg.batch_op_groups;
   if (next_group_idx < state.batch_op_groups.size()) {
      auto& next_group = state.batch_op_groups[next_group_idx];

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

      auto [encoded, out] = zpp::bits::data_out<char>();
      (void)out(attest);

      outposts_t outposts_tbl(get_self(), get_self().value);
      for (auto it = outposts_tbl.begin(); it != outposts_tbl.end(); ++it) {
         action(
            permission_level{"sysio.epoch"_n, "owner"_n},
            MSGCH_ACCOUNT,
            "queueout"_n,
            std::make_tuple(
               it->id,
               opp::types::ATTESTATION_TYPE_BATCH_OPERATOR_NEXT_GROUP,
               encoded
            )
         ).send();
      }
   }

   // Build outbound envelopes for each outpost
   {
      outposts_t outposts_tbl(get_self(), get_self().value);
      for (auto it = outposts_tbl.begin(); it != outposts_tbl.end(); ++it) {
         action(
            permission_level{"sysio.epoch"_n, "owner"_n},
            MSGCH_ACCOUNT,
            "buildenv"_n,
            std::make_tuple(it->id)
         ).send();
      }
   }

   // Cleanup old attestations/envelopes
   if (state.current_epoch_index > cfg.attestation_retention_epoch_count) {
      uint32_t before_epoch =
         state.current_epoch_index - cfg.attestation_retention_epoch_count;
      action(
         permission_level{"sysio.epoch"_n, "owner"_n},
         MSGCH_ACCOUNT,
         "cleanup"_n,
         std::make_tuple(before_epoch)
      ).send();
   }
}

// ---------------------------------------------------------------------------
//  initgroups — reads AVAILABLE batch ops from sysio.opreg
// ---------------------------------------------------------------------------
void epoch::initgroups() {
   require_auth(get_self());

   epochcfg_t cfg_tbl(get_self(), get_self().value);
   check(cfg_tbl.exists(), "epoch config not initialized");
   auto cfg = cfg_tbl.get();

   // Read AVAILABLE batch operators from sysio.opreg
   opreg::operators_t opreg_ops(OPREG_ACCOUNT, OPREG_ACCOUNT.value);
   auto status_idx = opreg_ops.get_index<"bystatus"_n>();

   // Collect AVAILABLE batch operators, separating staked from bootstrapped
   std::vector<std::pair<name, bool>> available_batch; // (account, is_bootstrapped)
   for (auto it = status_idx.lower_bound(
           static_cast<uint64_t>(OperatorStatus::OPERATOR_STATUS_ACTIVE)); // AVAILABLE = ACTIVE(3)
        it != status_idx.end() &&
        it->status == OperatorStatus::OPERATOR_STATUS_ACTIVE; ++it) {
      if (it->type == OperatorType::OPERATOR_TYPE_BATCH) {
         available_batch.push_back({it->account, it->is_bootstrapped});
      }
   }

   check(available_batch.size() >= cfg.batch_operator_minimum_active,
         "not enough available batch operators for group assignment");

   // Sort: staked operators first (is_bootstrapped=false), then bootstrapped.
   // Within each group, sort by account name for determinism.
   std::sort(available_batch.begin(), available_batch.end(),
      [](const auto& a, const auto& b) {
         if (a.second != b.second) return !a.second; // staked (false) before bootstrapped (true)
         return a.first < b.first; // deterministic by name within each category
      });

   // Trim to exactly batch_operator_minimum_active
   available_batch.resize(cfg.batch_operator_minimum_active);

   // Extract just names
   std::vector<name> batch_names;
   batch_names.reserve(available_batch.size());
   for (const auto& p : available_batch) {
      batch_names.push_back(p.first);
   }

   // Even/odd interleave shuffle
   std::vector<name> even_list, odd_list;
   for (size_t i = 0; i < batch_names.size(); ++i) {
      if (i % 2 == 0) {
         even_list.push_back(batch_names[i]);
      } else {
         odd_list.push_back(batch_names[i]);
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
      uint32_t end_idx = start + cfg.operators_per_epoch;
      for (uint32_t i = start; i < end_idx && i < shuffled.size(); ++i) {
         group.push_back(shuffled[i]);
      }
      new_groups.push_back(group);
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
//  regoutpost
// ---------------------------------------------------------------------------
void epoch::regoutpost(opp::types::ChainKind chain_kind, uint32_t chain_id) {
   require_auth(get_self());

   outposts_t outposts(get_self(), get_self().value);

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
