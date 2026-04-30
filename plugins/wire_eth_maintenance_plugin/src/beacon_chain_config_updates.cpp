#include <sysio/beacon_chain_config_updates.hpp>

#include <fc/io/json.hpp>
#include <fc/log/logger.hpp>

namespace sysio {

queue_updates beacon_chain_config_updates::compute_queue_updates(const fc::variant& queues_response) const {
   queue_updates result;

   constexpr uint64_t sec_per_day = 60 * 60 * 24;
   constexpr uint64_t max_withdraw_delay_sec = 180ull * sec_per_day; // 180-day sanity cap
   constexpr uint64_t max_entry_queue_days   = 365;

   const uint64_t exit_queue_buffer_seconds = exit_queue_buffer_days_ * sec_per_day;
   auto exit_eta = beacon_chain_detail::get_queue_length(queues_response, "exit_queue");
   if (!exit_eta)
      wlog("exit_queue EPA was not a finite number, defaulting to {}-day buffer only", exit_queue_buffer_days_);
   result.withdraw_delay_sec = exit_queue_buffer_seconds + exit_eta.value_or(0);

   auto deposit_eta = beacon_chain_detail::get_queue_length(queues_response, "deposit_queue");
   if (!deposit_eta) {
      wlog("deposit_queue EPA was not a finite number, defaulting to 1 day");
      result.entry_queue_days = 1;
   } else {
      ilog("deposit_queue len={} sec, sec_per_day={}", *deposit_eta, sec_per_day);
      result.entry_queue_days = *deposit_eta / sec_per_day;
   }

   if (result.withdraw_delay_sec && *result.withdraw_delay_sec > max_withdraw_delay_sec) {
      elog("withdraw_delay_sec={} exceeds sanity cap of {} seconds; skipping update",
           *result.withdraw_delay_sec, max_withdraw_delay_sec);
      result.withdraw_delay_sec.reset();
   }
   if (result.entry_queue_days && *result.entry_queue_days > max_entry_queue_days) {
      elog("entry_queue_days={} exceeds sanity cap of {} days; skipping update",
           *result.entry_queue_days, max_entry_queue_days);
      result.entry_queue_days.reset();
   }

   return result;
}

apy_updates beacon_chain_config_updates::compute_apy_updates(const fc::variant& ethstore_response) const {
   apy_updates result;

   constexpr auto     avgapr7d_field = "avgapr7d";
   constexpr uint64_t max_apy_bps    = 10000; // 100% cap

   auto apy_var = beacon_chain_detail::get_field_from_object(ethstore_response, avgapr7d_field);
   if (!apy_var) {
      elog("ethstore response did not have a {} field", avgapr7d_field);
   } else if (!apy_var->is_numeric()) {
      elog("ethstore response {} field was not numeric; skipping APY update", avgapr7d_field);
   } else {
      const double apr_fraction = apy_var->as_double();
      auto bps = beacon_chain_detail::apy_fraction_to_bps(apr_fraction);
      if (bps > max_apy_bps) {
         elog("apy_bps={} exceeds sanity cap of {}; skipping update", bps, max_apy_bps);
      } else {
         result.apy_bps = bps;
      }
   }

   return result;
}

beacon_chain_config_updates::beacon_chain_config_updates(beacon_chain_config_updates_deps deps,
                                                         uint64_t exit_queue_buffer_days)
   : deps_(std::move(deps)), exit_queue_buffer_days_(exit_queue_buffer_days) {}

void beacon_chain_config_updates::safely_confirm(std::string_view method,
                                                 const std::string& tx_hash) const {
   if (!deps_.confirm_tx) return;
   try {
      deps_.confirm_tx(method, tx_hash);
   } catch (const std::exception& e) {
      elog("confirm_tx for {} ({}) threw: {}", method, tx_hash, e.what());
   }
}

void beacon_chain_config_updates::operator()() const {
   try {
      ilog("beacon_chain_config_updates: fetching queue data");
      auto queues = deps_.fetch_queues();
      ilog("queues: {}", fc::json::to_string(queues, fc::time_point::maximum()));

      auto q = compute_queue_updates(queues);

      if (q.withdraw_delay_sec && deps_.send_set_withdraw_delay) {
         ilog("Sending setWithdrawDelay({} sec)", *q.withdraw_delay_sec);
         auto hash = deps_.send_set_withdraw_delay(*q.withdraw_delay_sec);
         if (!hash.empty()) {
            ilog("setWithdrawDelay tx sent, hash: {}", hash);
            safely_confirm("setWithdrawDelay", hash);
         }
      }

      if (q.entry_queue_days && deps_.send_set_entry_queue) {
         ilog("Sending setEntryQueue({} days)", *q.entry_queue_days);
         auto hash = deps_.send_set_entry_queue(*q.entry_queue_days);
         if (!hash.empty()) {
            ilog("setEntryQueue tx sent, hash: {}", hash);
            safely_confirm("setEntryQueue", hash);
         }
      }

      if (deps_.send_update_apy_bps) {
         ilog("beacon_chain_config_updates: fetching APY data");
         auto ethstore = deps_.fetch_apy();
         ilog("ethstore: {}", fc::json::to_string(ethstore, fc::time_point::maximum()));

         auto a = compute_apy_updates(ethstore);

         if (a.apy_bps) {
            ilog("Sending updateApyBPS({} bps)", *a.apy_bps);
            auto hash = deps_.send_update_apy_bps(*a.apy_bps);
            if (!hash.empty()) {
               ilog("updateApyBPS tx sent, hash: {}", hash);
               safely_confirm("updateApyBPS", hash);
            }
         }
      }

   } catch (const std::exception& e) {
      elog("beacon_chain_config_updates failed: {}", e.what());
   }
}

} // namespace sysio
