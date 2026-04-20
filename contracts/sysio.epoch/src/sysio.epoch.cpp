#include <sysio.epoch/sysio.epoch.hpp>
#include <sysio.opreg/sysio.opreg.hpp>
#include <sysio.authex/sysio.authex.hpp>
#include <sysio/opp/attestations/attestations.pb.hpp>
#include <zpp_bits.h>

namespace sysio {

using opp::types::OperatorType;
using opp::types::AttestationType;
using opp::types::OperatorStatus;

// Read-only mirror of sysio.authex::links table for cross-contract reads.
namespace authex_readonly {

struct links_key {
   uint64_t key;
   SYSLIB_SERIALIZE(links_key, (key))
};

struct links_row {
   uint64_t                 key;
   name                     username;
   fc::crypto::chain_kind_t chain_kind;
   public_key               pub_key;

   uint64_t by_name() const { return username.value; }

   SYSLIB_SERIALIZE(links_row, (key)(username)(chain_kind)(pub_key))
};

using links_t = sysio::kv::table<"links"_n, links_key, links_row,
   sysio::kv::index<"byname"_n,
      sysio::const_mem_fun<links_row, uint64_t, &links_row::by_name>>
>;

} // namespace authex_readonly

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

   epochcfg_t cfg_tbl(get_self());
   epoch_config cfg = cfg_tbl.get_or_default(epoch_config{});
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
   epochcfg_t cfg_tbl(get_self());
   check(cfg_tbl.exists(), "epoch config not initialized");
   auto cfg = cfg_tbl.get();

   epochstate_t state_tbl(get_self());
   epoch_state state = state_tbl.get_or_default(epoch_state{});

   check(!state.is_paused, "epoch advancement is paused");

   // Post-genesis: only sysio.msgch (via evalcons consensus) may advance.
   // Genesis (epoch 0→1): permissionless for bootstrap.
   if (state.current_epoch_index > 0) {
      check(has_auth(MSGCH_ACCOUNT) || has_auth(get_self()),
            "only sysio.msgch may advance the epoch after genesis");
   }

   // Wall-clock minimum: don't advance before next_epoch_start
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

   // Queue OPERATORS attestation (full roster with authex chain addresses) for each outpost.
   // IMPORTANT: Must come before BATCH_OPERATOR_GROUPS so that the ETH outpost's
   // _handleOperators populates operatorEthAddress before _handleBatchOperatorGroups
   // looks up those addresses.
   {
      opp::attestations::Operators ops_attest;
      opreg::operators_t opreg_ops(OPREG_ACCOUNT);
      authex_readonly::links_t authex_links(AUTHEX_ACCOUNT);
      auto links_by_name = authex_links.get_index<"byname"_n>();

      for (auto it = opreg_ops.begin(); it != opreg_ops.end(); ++it) {
         opp::attestations::OperatorEntry entry;
         entry.account.name = it->account.to_string();
         entry.type = it->type;
         entry.status = it->status;

         // Collect all authex-linked chain addresses for this operator.
         // Store raw public key bytes from the variant (33 bytes for EM/secp256k1,
         // 32 bytes for ED/Ed25519).
         auto link_it = links_by_name.lower_bound(it->account.value);
         while (link_it != links_by_name.end() && link_it->username == it->account) {
            opp::types::ChainAddress chain_addr;
            chain_addr.kind = static_cast<opp::types::ChainKind>(link_it->chain_kind);

            std::visit([&](const auto& key_data) {
               using T = std::decay_t<decltype(key_data)>;
               if constexpr (std::is_same_v<T, webauthn_public_key>) {
                  // EM (secp256k1 compressed) — 33 bytes in key.key
                  chain_addr.address.assign(key_data.key.begin(), key_data.key.end());
               } else if constexpr (std::is_same_v<T, ed_public_key>) {
                  // ED (Ed25519) — 32 bytes
                  chain_addr.address.assign(
                     reinterpret_cast<const char*>(key_data.data()),
                     reinterpret_cast<const char*>(key_data.data() + key_data.size()));
               } else if constexpr (std::is_same_v<T, ecc_public_key>) {
                  // K1/R1 (secp256k1/P-256 compressed) — 33 bytes
                  chain_addr.address.assign(key_data.begin(), key_data.end());
               }
               // Skip BLS keys — not used for chain address linking
            }, link_it->pub_key);

            entry.addresses.push_back(std::move(chain_addr));
            ++link_it;
         }

         ops_attest.operators.push_back(std::move(entry));
      }

      std::vector<char> encoded;
      auto out = zpp::bits::out{encoded, zpp::bits::no_size{}};
      (void)out(ops_attest);

      outposts_t outposts_tbl(get_self());
      for (auto it = outposts_tbl.begin(); it != outposts_tbl.end(); ++it) {
         action(
            permission_level{"sysio.epoch"_n, "owner"_n},
            MSGCH_ACCOUNT,
            "queueout"_n,
            std::make_tuple(
               it->id,
               opp::types::ATTESTATION_TYPE_OPERATORS,
               encoded
            )
         ).send();
      }
   }

   // Queue BATCH_OPERATOR_GROUPS attestation for each outpost.
   // Includes ALL groups and the active group index for the new epoch.
   {
      opp::attestations::BatchOperatorGroups attest;
      attest.active_group_index = zpp::bits::vuint32_t{state.current_batch_op_group};
      attest.epoch_index = zpp::bits::vuint32_t{state.current_epoch_index};
      for (auto& group : state.batch_op_groups) {
         opp::attestations::BatchOperatorGroup grp;
         for (auto& op_name : group) {
            opp::types::ChainAddress addr;
            addr.kind = opp::types::CHAIN_KIND_WIRE;
            auto name_str = op_name.to_string();
            addr.address.assign(name_str.begin(), name_str.end());
            grp.operators.push_back(std::move(addr));
         }
         attest.groups.push_back(std::move(grp));
      }

      std::vector<char> encoded;
      auto out = zpp::bits::out{encoded, zpp::bits::no_size{}};
      (void)out(attest);

      outposts_t outposts_tbl(get_self());
      for (auto it = outposts_tbl.begin(); it != outposts_tbl.end(); ++it) {
         action(
            permission_level{"sysio.epoch"_n, "owner"_n},
            MSGCH_ACCOUNT,
            "queueout"_n,
            std::make_tuple(
               it->id,
               opp::types::ATTESTATION_TYPE_BATCH_OPERATOR_GROUPS,
               encoded
            )
         ).send();
      }
   }

   // Build outbound envelopes for each outpost
   {
      outposts_t outposts_tbl(get_self());
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

   epochcfg_t cfg_tbl(get_self());
   check(cfg_tbl.exists(), "epoch config not initialized");
   auto cfg = cfg_tbl.get();

   // Read AVAILABLE batch operators from sysio.opreg
   opreg::operators_t opreg_ops(OPREG_ACCOUNT);
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
   epochstate_t state_tbl(get_self());
   epoch_state state = state_tbl.get_or_default(epoch_state{});
   state.batch_op_groups = new_groups;
   state_tbl.set(state, get_self());
}

// ---------------------------------------------------------------------------
//  regoutpost
// ---------------------------------------------------------------------------
void epoch::regoutpost(opp::types::ChainKind chain_kind, uint32_t chain_id) {
   require_auth(get_self());

   outposts_t outposts(get_self());

   auto chain_idx = outposts.get_index<"bychain"_n>();
   uint64_t composite = (static_cast<uint64_t>(chain_kind) << 32) | chain_id;
   auto it = chain_idx.find(composite);
   check(it == chain_idx.end(), "outpost already registered");

   uint64_t next_id = outposts.available_primary_key();

   outpost_info row{};
   row.id                 = next_id;
   row.chain_kind         = chain_kind;
   row.chain_id           = chain_id;
   row.last_inbound_epoch = 0;
   row.last_outbound_epoch = 0;
   outposts.emplace(get_self(), outpost_key{next_id}, row);
}

// ---------------------------------------------------------------------------
//  pause / unpause
// ---------------------------------------------------------------------------
void epoch::pause() {
   require_auth(CHALG_ACCOUNT);

   epochstate_t state_tbl(get_self());
   epoch_state state = state_tbl.get_or_default(epoch_state{});
   state.is_paused = true;
   state_tbl.set(state, get_self());
}

void epoch::unpause() {
   require_auth(CHALG_ACCOUNT);

   epochstate_t state_tbl(get_self());
   check(state_tbl.exists(), "epoch state not initialized");
   auto state = state_tbl.get();
   state.is_paused = false;
   state_tbl.set(state, get_self());
}

} // namespace sysio
