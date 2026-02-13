#pragma once

#include <sysio.depot/depot.tables.hpp>

namespace sysio {

   class [[sysio::contract("sysio.depot")]] depot : public contract {
   public:
      using contract::contract;

      // ── Initialization ────────────────────────────────────────────────────
      [[sysio::action]]
      void init(chain_kind_t chain_id, name token_contract);

      // ── FR-800: Crank Execution ───────────────────────────────────────────
      [[sysio::action]]
      void crank(name operator_account);

      // ── FR-100: OPP Inbound ───────────────────────────────────────────────
      [[sysio::action]]
      void submitchain(name             operator_account,
                       uint64_t         epoch_number,
                       checksum256      epoch_hash,
                       checksum256      prev_epoch_hash,
                       checksum256      merkle_root,
                       std::vector<char> signature);

      [[sysio::action]]
      void uploadmsgs(name              operator_account,
                      uint64_t          epoch_number,
                      std::vector<char> messages,
                      std::vector<char> merkle_proofs);

      // ── FR-200: OPP Outbound ──────────────────────────────────────────────
      [[sysio::action]]
      void emitchain(name operator_account, uint64_t epoch_number);

      // ── FR-400: Underwriting ──────────────────────────────────────────────
      [[sysio::action]]
      void uwintent(name              underwriter,
                    uint64_t          message_id,
                    asset             source_amount,
                    asset             target_amount,
                    chain_kind_t      source_chain,
                    chain_kind_t      target_chain,
                    std::vector<char> source_sig,
                    std::vector<char> target_sig);

      [[sysio::action]]
      void uwconfirm(name operator_account, uint64_t ledger_entry_id);

      [[sysio::action]]
      void uwcancel(name operator_account, uint64_t ledger_entry_id, std::string reason);

      [[sysio::action]]
      void uwexpire();

      // ── FR-500: Challenge ─────────────────────────────────────────────────
      [[sysio::action]]
      void challenge(name challenger, uint64_t epoch_number, std::vector<char> evidence);

      [[sysio::action]]
      void chalresp(name operator_account, uint64_t challenge_id, std::vector<char> response_data);

      [[sysio::action]]
      void chalresolve(name        proposer,
                       uint64_t    challenge_id,
                       checksum256 original_hash,
                       checksum256 round1_hash,
                       checksum256 round2_hash);

      [[sysio::action]]
      void chalvote(name voter, uint64_t challenge_id, bool approve);

      // ── FR-600: Operator Lifecycle ────────────────────────────────────────
      [[sysio::action]]
      void regoperator(name              wire_account,
                       operator_type_t   op_type,
                       std::vector<char> secp256k1_pubkey,
                       std::vector<char> ed25519_pubkey,
                       asset             collateral);

      [[sysio::action]]
      void unregop(name wire_account);

      [[sysio::action]]
      void activateop(name wire_account);

      [[sysio::action]]
      void exitop(name wire_account);

      [[sysio::action]]
      void slashop(name wire_account, std::string reason);

      // ── FR-700: Reserve & Swap ────────────────────────────────────────────
      [[sysio::action]]
      void setreserve(name authority, asset reserve_total, asset wire_equivalent);

      [[sysio::action]]
      void updreserve(name operator_account, symbol token_sym, int64_t delta);

      // ── FR-900: Swap Quote (read-only) ────────────────────────────────────
      [[sysio::action]]
      void getquote(symbol source_sym, symbol target_sym, asset amount);

      // ── FR-1000: Oneshot Warrant Conversion ───────────────────────────────
      [[sysio::action]]
      void oneshot(name beneficiary, asset amount);

      // ── Action wrappers ───────────────────────────────────────────────────
      using init_action         = action_wrapper<"init"_n,         &depot::init>;
      using crank_action        = action_wrapper<"crank"_n,        &depot::crank>;
      using submitchain_action  = action_wrapper<"submitchain"_n,  &depot::submitchain>;
      using uploadmsgs_action   = action_wrapper<"uploadmsgs"_n,   &depot::uploadmsgs>;
      using emitchain_action    = action_wrapper<"emitchain"_n,    &depot::emitchain>;
      using uwintent_action     = action_wrapper<"uwintent"_n,     &depot::uwintent>;
      using uwconfirm_action    = action_wrapper<"uwconfirm"_n,    &depot::uwconfirm>;
      using uwcancel_action     = action_wrapper<"uwcancel"_n,     &depot::uwcancel>;
      using uwexpire_action     = action_wrapper<"uwexpire"_n,     &depot::uwexpire>;
      using challenge_action    = action_wrapper<"challenge"_n,    &depot::challenge>;
      using chalresp_action     = action_wrapper<"chalresp"_n,     &depot::chalresp>;
      using chalresolve_action  = action_wrapper<"chalresolve"_n,  &depot::chalresolve>;
      using chalvote_action     = action_wrapper<"chalvote"_n,     &depot::chalvote>;
      using regoperator_action  = action_wrapper<"regoperator"_n,  &depot::regoperator>;
      using unregop_action      = action_wrapper<"unregop"_n,      &depot::unregop>;
      using activateop_action   = action_wrapper<"activateop"_n,   &depot::activateop>;
      using exitop_action       = action_wrapper<"exitop"_n,       &depot::exitop>;
      using slashop_action      = action_wrapper<"slashop"_n,      &depot::slashop>;
      using setreserve_action   = action_wrapper<"setreserve"_n,   &depot::setreserve>;
      using updreserve_action   = action_wrapper<"updreserve"_n,   &depot::updreserve>;
      using getquote_action     = action_wrapper<"getquote"_n,     &depot::getquote>;
      using oneshot_action      = action_wrapper<"oneshot"_n,      &depot::oneshot>;

   private:
      // ── Internal helpers ──────────────────────────────────────────────────
      depot_global_state get_state();
      void               set_state(const depot_global_state& s);

      void     verify_elected(name operator_account, uint64_t epoch_number);
      void     evaluate_consensus(uint64_t epoch_number);
      void     process_ready_messages();
      void     expire_underwriting_locks();
      void     elect_operators_for_epoch(uint64_t next_epoch);
      void     process_assertion(uint64_t message_number, assertion_type_t type, const std::vector<char>& payload);
      void     queue_outbound_message(assertion_type_t type, const std::vector<char>& payload);
      void     mark_epoch_valid(uint64_t chain_scope, uint64_t epoch_number);
      void     slash_minority_operators(uint64_t chain_scope, uint64_t epoch_number,
                                        const checksum256& consensus_hash);
      void     enter_challenge_state(uint64_t epoch_number);
      void     resume_from_challenge();
      void     distribute_slash(uint64_t operator_id, name challenger);
      uint64_t calculate_swap_rate(symbol source, symbol target, asset amount);
      bool     check_rate_threshold(uint64_t rate_bps, uint64_t threshold_bps);

      uint64_t scope() {
         auto s = get_state();
         return uint64_t(s.chain_id);
      }
   };

} // namespace sysio
