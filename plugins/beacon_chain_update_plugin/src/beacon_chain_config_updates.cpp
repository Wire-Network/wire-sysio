#include <sysio/beacon_chain_config_updates.hpp>

#include <fc/io/json.hpp>
#include <fc/log/logger.hpp>

namespace sysio {

queue_updates compute_queue_updates(const fc::variant& queues_response) {
   queue_updates result;

   constexpr uint64_t nine_days_sec = 60 * 60 * 24 * 9;
   auto exit_eta = beacon_chain_detail::get_queue_length(queues_response, "exit_queue");
   if (!exit_eta)
      wlog("exit_queue EPA was not a finite number, defaulting to 9-day buffer only");
   result.withdraw_delay_sec = nine_days_sec + exit_eta.value_or(0);

   auto deposit_eta = beacon_chain_detail::get_queue_length(queues_response, "deposit_queue");
   constexpr uint64_t seconds_per_day = 60 * 60 * 24;
   if (!deposit_eta) {
      wlog("deposit_queue EPA was not a finite number, defaulting to 1 day");
      result.entry_queue_days = 1;
   } else {
      ilog("deposit_queue len={} sec, sec_per_day={}", *deposit_eta, seconds_per_day);
      result.entry_queue_days = *deposit_eta / seconds_per_day;
   }

   return result;
}

apy_updates compute_apy_updates(const fc::variant& ethstore_response) {
   apy_updates result;

   constexpr auto avgapr7d_field = "avgapr7d";
   auto apy_var = beacon_chain_detail::get_field_from_object(ethstore_response, avgapr7d_field);
   if (!apy_var) {
      elog("ethstore response did not have a {} field", avgapr7d_field);
   } else {
      double apr_fraction = 0.0;
      if (apy_var->is_double())
         apr_fraction = apy_var->as_double();
      result.apy_bps = beacon_chain_detail::apy_fraction_to_bps(apr_fraction);
   }

   return result;
}

beacon_chain_config_updates::beacon_chain_config_updates(beacon_chain_config_updates_deps deps)
   : deps_(std::move(deps)) {}

void beacon_chain_config_updates::operator()() const {
   try {
      std::vector<pending_tx> pending;

      ilog("beacon_chain_config_updates: fetching queue data");
      auto queues = deps_.fetch_queues();
      ilog("queues: {}", fc::json::to_string(queues, fc::time_point::maximum()));

      auto q = compute_queue_updates(queues);

      if (q.withdraw_delay_sec && deps_.send_set_withdraw_delay) {
         ilog("Sending setWithdrawDelay({} sec)", *q.withdraw_delay_sec);
         auto hash = deps_.send_set_withdraw_delay(*q.withdraw_delay_sec);
         if (!hash.empty()) {
            ilog("setWithdrawDelay tx sent, hash: {}", hash);
            pending.push_back({"setWithdrawDelay", std::move(hash)});
         }
      }

      if (q.entry_queue_days && deps_.send_set_entry_queue) {
         ilog("Sending setEntryQueue({} days)", *q.entry_queue_days);
         auto hash = deps_.send_set_entry_queue(*q.entry_queue_days);
         if (!hash.empty()) {
            ilog("setEntryQueue tx sent, hash: {}", hash);
            pending.push_back({"setEntryQueue", std::move(hash)});
         }
      }

      ilog("beacon_chain_config_updates: fetching APY data");
      auto ethstore = deps_.fetch_apy();
      ilog("ethstore: {}", fc::json::to_string(ethstore, fc::time_point::maximum()));

      auto a = compute_apy_updates(ethstore);

      if (a.apy_bps && deps_.send_update_apy_bps) {
         ilog("Sending updateApyBPS({} bps)", *a.apy_bps);
         auto hash = deps_.send_update_apy_bps(*a.apy_bps);
         if (!hash.empty()) {
            ilog("updateApyBPS tx sent, hash: {}", hash);
            pending.push_back({"updateApyBPS", std::move(hash)});
         }
      }

      if (!pending.empty() && deps_.confirm_txs)
         deps_.confirm_txs(pending);

   } catch (const std::exception& e) {
      elog("beacon_chain_config_updates failed: {}", e.what());
   }
}

} // namespace sysio
