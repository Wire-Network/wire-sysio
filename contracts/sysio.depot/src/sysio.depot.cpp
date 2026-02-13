#include <sysio.depot/sysio.depot.hpp>

namespace sysio {

// ── State helpers ────────────────────────────────────────────────────────────

depot_global_state depot::get_state() {
   depot_state_singleton state_tbl(get_self(), get_self().value);
   check(state_tbl.exists(), "depot: contract not initialized");
   return state_tbl.get();
}

void depot::set_state(const depot_global_state& s) {
   depot_state_singleton state_tbl(get_self(), get_self().value);
   state_tbl.set(s, get_self());
}

// ── init ─────────────────────────────────────────────────────────────────────

void depot::init(chain_kind_t chain_id, name token_contract) {
   require_auth(get_self());

   depot_state_singleton state_tbl(get_self(), get_self().value);
   check(!state_tbl.exists() || !state_tbl.get().initialized,
         "depot: already initialized");

   check(chain_id != fc::crypto::chain_kind_unknown && chain_id != fc::crypto::chain_kind_wire,
         "depot: chain_id must be an external chain (ethereum, solana, sui)");
   check(is_account(token_contract), "depot: token_contract account does not exist");

   depot_global_state s;
   s.state          = depot_state_active;
   s.chain_id       = chain_id;
   s.current_epoch  = 0;
   s.next_epoch     = 1;
   s.next_msg_out   = 0;
   s.last_crank_time = current_time_point();
   s.token_contract = token_contract;
   s.initialized    = true;

   state_tbl.set(s, get_self());
}

// ── crank  (FR-801) ──────────────────────────────────────────────────────────

void depot::crank(name operator_account) {
   require_auth(operator_account);

   auto s = get_state();
   check(s.state != depot_state_paused, "depot: system is paused, manual intervention required");

   // Verify the caller is an elected batch operator for the current epoch
   verify_elected(operator_account, s.current_epoch);

   // Step 1: Expire underwriting locks (FR-402)
   expire_underwriting_locks();

   // Step 2: Process messages previously marked as READY (FR-801.3)
   if (s.state == depot_state_active) {
      process_ready_messages();
   }

   // Step 3: Elect 7 active batch operators for NEXT message chain (FR-801.2 / FR-604)
   elect_operators_for_epoch(s.next_epoch);

   // Step 4: Advance epoch
   s.current_epoch  = s.next_epoch;
   s.next_epoch     = s.next_epoch + 1;
   s.last_crank_time = current_time_point();

   set_state(s);
}

// ── process_ready_messages (internal) ────────────────────────────────────────

void depot::process_ready_messages() {
   auto s = get_state();
   uint64_t chain_scope = uint64_t(s.chain_id);

   opp_in_table msgs(get_self(), chain_scope);
   auto by_status = msgs.get_index<"bystatus"_n>();
   auto it = by_status.lower_bound(uint64_t(message_status_ready));

   while (it != by_status.end() && it->status == message_status_ready) {
      process_assertion(it->message_number, it->assertion_type, it->payload);
      it = by_status.erase(it);
   }
}

// ── process_assertion (internal) ─────────────────────────────────────────────

void depot::process_assertion(uint64_t message_number, assertion_type_t type,
                              const std::vector<char>& payload) {
   // Dispatch based on assertion type
   switch (type) {
      case assertion_type_balance_sheet: {
         // FR-109: Update reserve state from balance sheet
         // Payload: chain_id + amounts (symbol + asset pairs)
         // TODO: Deserialize and update reserves_table
         break;
      }
      case assertion_type_stake_update: {
         // FR-109: Handle principal deposit or withdrawal (0xEE00)
         // TODO: Process stake update
         break;
      }
      case assertion_type_yield_reward: {
         // FR-109: Native chain yield reward distribution (0xEE01)
         // TODO: Calculate and schedule reward distribution
         break;
      }
      case assertion_type_wire_purchase: {
         // FR-109: $WIRE token purchase (0xEE02)
         // TODO: Process purchase assertion
         break;
      }
      case assertion_type_operator_registration: {
         // FR-109 / FR-602: Operator registered/deregistered (0xEE03)
         // TODO: Update known_operators table
         break;
      }
      case assertion_type_challenge_response: {
         // FR-108: Challenge accept/reject/no_challenge (0xEE04)
         // Always processed regardless of consensus status
         // TODO: Route to challenge handler
         break;
      }
      case assertion_type_slash_operator: {
         // FR-109: Slash operator outbound (0xEE05)
         // TODO: Process slash propagation
         break;
      }
      default: {
         check(false, "depot: unknown assertion type");
         break;
      }
   }
}

// ── expire_underwriting_locks (internal, FR-402) ─────────────────────────────

void depot::expire_underwriting_locks() {
   auto s = get_state();
   uint64_t chain_scope = uint64_t(s.chain_id);

   underwrite_table ledger(get_self(), chain_scope);
   auto by_expiry = ledger.get_index<"byexpiry"_n>();
   uint64_t now_sec = current_time_point().sec_since_epoch();

   auto it = by_expiry.begin();
   while (it != by_expiry.end() && it->unlock_at.sec_since_epoch() <= now_sec) {
      if (it->status == underwrite_status_intent_submitted ||
          it->status == underwrite_status_intent_confirmed) {
         by_expiry.modify(it, same_payer, [&](auto& e) {
            e.status = underwrite_status_expired;
         });
      }
      ++it;
   }
}

// ── queue_outbound_message (internal) ────────────────────────────────────────

void depot::queue_outbound_message(assertion_type_t type, const std::vector<char>& payload) {
   auto s = get_state();
   uint64_t chain_scope = uint64_t(s.chain_id);

   opp_out_table out(get_self(), chain_scope);
   uint64_t next_num = s.next_msg_out;

   out.emplace(get_self(), [&](auto& m) {
      m.message_number = next_num;
      m.assertion_type = type;
      m.payload        = payload;
   });

   s.next_msg_out = next_num + 1;
   set_state(s);
}

} // namespace sysio
