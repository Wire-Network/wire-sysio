#pragma once

#include <format>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <fc/exception/exception.hpp>

#include <sysio/outpost_client/outpost_client.hpp>

namespace sysio::test {

/**
 * @brief Recording/scripted implementation of outpost_client for unit tests.
 *
 * Records every virtual call (method name + args) into public vectors, and
 * lets tests script the response. Thread-safe so it can be driven from both
 * the outbound and inbound jobs simultaneously.
 */
class mock_outpost_client : public sysio::outpost_client {
public:
   struct outbound_call {
      uint32_t          epoch_index = 0;
      std::vector<char> envelope_bytes;
      fc::microseconds  deadline;
   };
   struct inbound_call {
      uint32_t         epoch_index = 0;
      fc::microseconds deadline;
   };

   mock_outpost_client(sysio::opp::types::ChainKind kind,
                       uint64_t                     id,
                       uint32_t                     cid)
      : _kind(kind), _outpost_id(id), _chain_id(cid) {}

   sysio::opp::types::ChainKind chain_kind() const override { return _kind; }
   uint64_t                     outpost_id() const override { return _outpost_id; }
   uint32_t                     chain_id()   const override { return _chain_id; }
   std::string                  to_string()  const override {
      return std::format("{}:{}:{}",
                         _outpost_id,
                         sysio::opp::types::ChainKind_Name(_kind),
                         _chain_id);
   }

   /// Deliver response — can be set to either a scripted string or a functor
   /// that produces responses / throws per-call.
   std::function<std::string(const outbound_call&)> deliver_response =
      [](const outbound_call&) { return std::string{"mock-tx-id"}; };

   std::function<std::vector<char>(const inbound_call&)> inbound_response =
      [](const inbound_call&) { return std::vector<char>{}; };

   std::string deliver_outbound_envelope(uint32_t                 epoch_index,
                                         const std::vector<char>& envelope_bytes,
                                         fc::microseconds         deadline) override {
      outbound_call call{epoch_index, envelope_bytes, deadline};
      {
         std::lock_guard<std::mutex> lock(_mx);
         outbound_calls.push_back(call);
      }
      return deliver_response(call);
   }

   std::vector<char> read_inbound_envelope(uint32_t         epoch_index,
                                           fc::microseconds deadline) override {
      inbound_call call{epoch_index, deadline};
      {
         std::lock_guard<std::mutex> lock(_mx);
         inbound_calls.push_back(call);
      }
      return inbound_response(call);
   }

   std::vector<outbound_call> outbound_calls;
   std::vector<inbound_call>  inbound_calls;

private:
   sysio::opp::types::ChainKind _kind;
   uint64_t                     _outpost_id;
   uint32_t                     _chain_id;
   mutable std::mutex           _mx;
};

} // namespace sysio::test
