#pragma once

#include <fc-lite/expected.hpp>
#include <fc/exception/exception.hpp>
#include <fc/variant.hpp>
#include <sysio/beacon_chain_update_detail.hpp>
#include <sysio/services/cron_service.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace sysio {

// Internal-only plain-data types: no FC_REFLECT declarations are provided because these
// are never serialized over the network or to disk - they exist to move values between
// the fetch/compute/transact/confirm steps within this plugin.

struct queue_updates {
   std::optional<uint64_t> withdraw_delay_sec;
   std::optional<uint64_t> entry_queue_days;
};

struct apy_updates {
   std::optional<uint64_t> apy_bps;
};

struct beacon_chain_config_updates_deps {
   std::function<fc::variant()> fetch_queues;
   std::function<fc::variant()> fetch_apy;
   std::function<std::string(uint64_t)> send_set_withdraw_delay;
   std::function<std::string(uint64_t)> send_set_entry_queue;
   std::function<std::string(uint64_t)> send_update_apy_bps;
   /// Called once per successful send with the contract method name and the tx hash
   /// returned by the corresponding `send_*` callback. The implementation is responsible
   /// for blocking until the tx is confirmed (or determining that confirmation is
   /// impossible) and reporting the outcome via logging - it must not throw to indicate
   /// confirmation failure, since the surrounding orchestration treats throws as bugs.
   std::function<void(std::string_view method, const std::string& tx_hash)> confirm_tx;
};

class beacon_chain_config_updates {
public:
   beacon_chain_config_updates(beacon_chain_config_updates_deps deps, uint64_t exit_queue_buffer_days);
   void operator()() const;

   queue_updates compute_queue_updates(const fc::variant& queues_response) const;
   apy_updates   compute_apy_updates(const fc::variant& ethstore_response) const;

private:
   /// Invoke `deps_.confirm_tx` with the given (method, hash) pair, swallowing any exception
   /// it raises so that one bad confirmation cannot prevent subsequent sends from running.
   void safely_confirm(std::string_view method, const std::string& tx_hash) const;

   beacon_chain_config_updates_deps deps_;
   const uint64_t exit_queue_buffer_days_;
};

} // namespace sysio
