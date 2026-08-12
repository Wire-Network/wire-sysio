#pragma once

#include <cstddef>
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

/// Cross-chain hard cap for a serialized OPP envelope accepted from an outpost.
/// This mirrors the Solana program's `MAX_ENVELOPE_BYTES` and the e2e-supported
/// WIRE / Ethereum / Solana envelope boundary.
inline constexpr size_t OPP_MAX_ENVELOPE_BYTES = 65'536;

/**
 * @brief Chain-agnostic facade for OPP delivery on a single external outpost.
 *
 * This is the SPI (Service Provider Interface) between `batch_operator_plugin`
 * and the chain-specific `outpost_ethereum_client_plugin` /
 * `outpost_solana_client_plugin`. The orchestrating `outpost_opp_job` holds an
 * `outpost_client_ptr` and calls only the virtuals below — it never interprets
 * an ETH address, Solana public key, PDA, or signature-provider format.
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
   virtual uint64_t chain_code() const = 0;

   /// Numeric chain id on the target chain. Anvil = 31337, ETH mainnet = 1,
   /// Solana = 0 (Solana has no numeric chain id; clusters are identified by
   /// genesis hash).
   virtual uint32_t chain_id() const = 0;

   /**
    * @brief Raw chain-native address that authenticates this client's writes.
    *
    * Ethereum returns the 20-byte address derived from the configured
    * transaction signer. Solana returns the configured signer's 32-byte
    * public key. Underwriter UIC construction signs this value into
    * `uw_ext_chain_addr`, so the canonical payload contains no omitted
    * default byte field and records the local transaction signer as signed
    * metadata. The outpost binds this field and the claimed WIRE account to
    * the authenticated caller and its current ACTIVE underwriter roster row.
    *
    * @return Opaque chain-native address bytes; 20 bytes for Ethereum and 32
    *         bytes for Solana.
    */
   virtual std::vector<uint8_t> authenticated_caller_address() const = 0;

   /// Human-readable identifier safe to embed in log lines and metrics.
   /// Canonical format: `{chain_code}:{ChainKind_Name}:{chain_id}`
   /// e.g. `"0:CHAIN_KIND_EVM:31337"` or `"1:CHAIN_KIND_SVM:0"`.
   ///
   /// The default implementation derives the string from the other three
   /// getters — concretes only override when they want a chain-specific
   /// supplement (e.g. cluster name for Solana). Virtual, not pure.
   virtual std::string to_string() const {
      return std::format("{}:{}:{}",
                         chain_code(),
                         sysio::opp::types::ChainKind_Name(chain_kind()),
                         chain_id());
   }

   /**
    * @brief OPP OUTBOUND — submit a single envelope to the remote chain.
    *
    * Must enforce `deadline` internally; a hung chain RPC must not block the
    * caller beyond this duration. Solana may chunk one envelope across
    * multiple `epoch_in` transactions; the consensus-reaching transaction
    * performs the outpost's outbound emit internally.
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
    * @brief OPP INBOUND — pull the envelope the remote chain has produced for
    *        this epoch and return its raw protobuf bytes.
    *
    * Filters by `epoch_index` internally. Ethereum and Solana each expose a
    * single latest-outbound storage slot, so a poll may still observe the
    * preceding epoch until the consensus-reaching delivery overwrites it.
    * Delivering that stale envelope to `sysio.msgch::deliver` trips an
    * `envelope epoch_index mismatch` assertion.
    *
    * @param epoch_index  Only envelopes whose `epoch_index` field matches this
    *                     value are returned; all others are silently dropped.
    * @param deadline     Upper bound on the total time spent talking to the
    *                     remote chain for this call.
    * @return Raw `opp::Envelope` bytes ready for `sysio.msgch::deliver`, or an
    *         empty vector if the latest slot did not match.
    * @throws fc::exception on RPC failure or deadline expiry.
    */
   virtual std::vector<char> read_inbound_envelope(uint32_t         epoch_index,
                                                   fc::microseconds deadline) = 0;

   /**
    * @brief UNDERWRITER COMMIT — submit a signed `UnderwriteIntentCommit`
    *        (UIC) through an ACTIVE-role-gated outpost relay.
    *
    * Called by the underwriter plugin (or any future plugin that issues
    * outpost-side commits) to deliver a signed intent without the caller
    * knowing the outpost's contract surface, ABI / IDL layout, or message
    * encoding. The chain-specific concrete resolves which contract or
    * program action to invoke, how to encode the bytes for the wire, and
    * how to await on-chain confirmation. The current outposts accept only a
    * canonically encoded UIC whose claimed WIRE account and external address
    * match the authenticated caller's current ACTIVE underwriter roster row.
    * They queue the original validated bytes unchanged. The WIRE depot remains
    * authoritative for validating the embedded permission signature and bond.
    *
    * Returns only after on-chain inclusion + confirmations — the caller
    * uses the return value as a "this leg landed" signal before recording
    * the commit locally. Late-arriving commits (after consensus has already
    * been reached for the underlying envelope) are benign no-ops on the
    * outpost side per `opp-consensus.md`; they still confirm here.
    *
    * @param uw_request_id  The depot's `sysio.uwrit::uwreqs` row id this
    *                       UIC is committing to. Used only for log
    *                       correlation; the on-chain call carries the original
    *                       validated UIC bytes.
    * @param uic_bytes      Serialized `UnderwriteIntentCommit` (protobuf
    *                       encoded, signed by an authorized WIRE K1, R1,
    *                       EM, or ED permission key).
    * @param deadline       Upper bound on the total time spent talking to
    *                       the remote chain for this call.
    * @return Chain-native tx id / signature suitable for logs.
    * @throws fc::exception on RPC failure, tx revert, or deadline expiry.
    */
   virtual std::string uw_commit(uint64_t                 uw_request_id,
                                 const std::vector<char>& uic_bytes,
                                 fc::microseconds         deadline) = 0;

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
