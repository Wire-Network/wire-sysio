#pragma once

#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <fc/exception/exception.hpp>
#include <fc/time.hpp>
#include <sysio/opp/opp.hpp>

namespace sysio {

/**
 * @brief Chain-agnostic facade for OPP delivery on a single external outpost.
 *
 * This is the SPI (Service Provider Interface) between `batch_operator_plugin`
 * and the chain-specific `outpost_ethereum_client_plugin` /
 * `outpost_solana_client_plugin`. The orchestrating `outpost_opp_job` holds an
 * `outpost_client_ptr` and calls only the virtuals below — it never sees an
 * ETH address, a Solana PDA, or a signature-provider format.
 *
 * Each concrete implementation owns its chain-specific machinery (opp contract
 * clients, program clients, signature providers) and is responsible for
 * enforcing the deadline passed to every RPC-bound call so that a hung remote
 * chain cannot starve the cron worker pool.
 */
class outpost_client {
public:
   virtual ~outpost_client() = default;

   /// Chain kind this outpost targets — used by the orchestrator only for
   /// diagnostics and for selecting the correct debug endpoint enum value.
   virtual sysio::opp::types::ChainKind chain_kind() const = 0;

   /// Outpost id assigned by `sysio.epoch::regoutpost`.
   virtual uint64_t outpost_id() const = 0;

   /// Numeric chain id on the target chain. Anvil = 31337, ETH mainnet = 1,
   /// Solana = 0 (Solana has no numeric chain id; clusters are identified by
   /// genesis hash).
   virtual uint32_t chain_id() const = 0;

   /// Human-readable identifier safe to embed in log lines and metrics.
   /// Canonical format: `{outpost_id}:{ChainKind_Name}:{chain_id}`
   /// e.g. `"0:CHAIN_KIND_ETHEREUM:31337"` or `"1:CHAIN_KIND_SOLANA:0"`.
   ///
   /// The default implementation derives the string from the other three
   /// getters — concretes only override when they want a chain-specific
   /// supplement (e.g. cluster name for Solana). Virtual, not pure.
   virtual std::string to_string() const {
      return std::format("{}:{}:{}",
                         outpost_id(),
                         sysio::opp::types::ChainKind_Name(chain_kind()),
                         chain_id());
   }

   /**
    * @brief OPP OUTBOUND — submit a single envelope to the remote chain.
    *
    * Must enforce `deadline` internally; a hung chain RPC must not block the
    * caller beyond this duration. Implementations that issue multiple chain
    * transactions (e.g. Solana's `epoch_in` + `emit_outbound_envelope` pair)
    * apply the deadline to the overall sequence.
    *
    * @param epoch_index     The current WIRE epoch this envelope belongs to.
    * @param envelope_bytes  Raw protobuf `opp::Envelope` bytes.
    * @param deadline        Upper bound on the total time spent talking to the
    *                        remote chain for this call.
    * @return Chain-native transaction id / signature suitable for logs.
    * @throws fc::exception on RPC failure or deadline expiry.
    */
   virtual std::string deliver_outbound_envelope(uint32_t                 epoch_index,
                                                 const std::vector<char>& envelope_bytes,
                                                 fc::microseconds         deadline) = 0;

   /**
    * @brief OPP INBOUND — pull envelope(s) the remote chain has produced for
    *        this epoch and return the concatenated raw protobuf bytes.
    *
    * Filters by `epoch_index` internally — both ETH's event log and Solana's
    * signature history retain stale envelopes from prior epochs, and delivering
    * a stale envelope to `sysio.msgch::deliver` trips an
    * `envelope epoch_index mismatch` assertion.
    *
    * @param epoch_index  Only envelopes whose `epoch_index` field matches this
    *                     value are returned; all others are silently dropped.
    * @param deadline     Upper bound on the total time spent talking to the
    *                     remote chain for this call.
    * @return Concatenated raw `opp::Envelope` bytes ready for
    *         `sysio.msgch::deliver`, or an empty vector if none matched.
    * @throws fc::exception on RPC failure or deadline expiry.
    */
   virtual std::vector<char> read_inbound_envelope(uint32_t         epoch_index,
                                                   fc::microseconds deadline) = 0;

protected:
   /// Throw `fc::timeout_exception` if the wall-clock has crossed `deadline_abs`.
   /// Called by concretes before each blocking RPC to bound how long a hung
   /// remote chain can occupy a cron worker. Uses `to_string()` as the label
   /// so the thrown message identifies the outpost instance.
   void throw_if_past_deadline(fc::time_point   deadline_abs,
                               std::string_view op) const {
      if (fc::time_point::now() >= deadline_abs) {
         FC_THROW_EXCEPTION(fc::timeout_exception,
                            "{}: deadline exceeded during {}",
                            to_string(),
                            std::string(op));
      }
   }
};

using outpost_client_ptr = std::shared_ptr<outpost_client>;

} // namespace sysio
