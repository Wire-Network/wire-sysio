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
#include <vector>

namespace sysio {

struct queue_updates {
   std::optional<uint64_t> withdraw_delay_sec;
   std::optional<uint64_t> entry_queue_days;
};

struct apy_updates {
   std::optional<uint64_t> apy_bps;
};

struct pending_tx {
   std::string method;
   std::string tx_hash;
};

struct beacon_chain_config_updates_deps {
   std::function<fc::variant()> fetch_queues;
   std::function<fc::variant()> fetch_apy;
   std::function<std::string(uint64_t)> send_set_withdraw_delay;
   std::function<std::string(uint64_t)> send_set_entry_queue;
   std::function<std::string(uint64_t)> send_update_apy_bps;
   std::function<void(const std::vector<pending_tx>&)> confirm_txs;
};

class beacon_chain_config_updates {
public:
   beacon_chain_config_updates(beacon_chain_config_updates_deps deps, uint64_t exit_queue_buffer_days);
   void operator()() const;

   queue_updates compute_queue_updates(const fc::variant& queues_response) const;
   apy_updates   compute_apy_updates(const fc::variant& ethstore_response) const;

private:
   beacon_chain_config_updates_deps deps_;
   const uint64_t exit_queue_buffer_days_;
};

} // namespace sysio
