#pragma once

#include <memory>

#include <fc/network/ethereum/ethereum_abi.hpp>
#include <fc/network/ethereum/ethereum_client.hpp>

#include <sysio/outpost_client/outpost_client.hpp>
#include <sysio/outpost_ethereum_client_plugin.hpp>

namespace sysio {

/**
 * @brief Ethereum concrete `outpost_client`.
 *
 * Composes the plugin-owned `ethereum_client_entry_t` (shared chain connection
 * + signature provider) with per-outpost OPP contract metadata (the
 * `OPP.sol` and `OPPInbound.sol` addresses) to provide the chain-agnostic SPI
 * to `outpost_opp_job`.
 *
 * Constructed by `outpost_ethereum_client_plugin::create_outpost_client` —
 * `batch_operator_plugin` never builds one directly; it just calls the factory.
 */
class outpost_ethereum_client : public outpost_client {
public:
   /// Construct an Ethereum outpost client.
   ///
   /// `inbound_read_block_tag` is the Ethereum block tag used by `read_inbound_envelope` when calling
   /// `getLatestOutboundEnvelope`. WIRE consensus on inbound is committed forward against this read, so a tip
   /// reorg past the chosen tag leaves WIRE state derived from external history that no longer exists. Default is
   /// `latest`; deployments that prefer safety over latency can dial up to `safe` or `finalized` via
   /// `--ethereum-inbound-read-block-tag`.
   outpost_ethereum_client(ethereum_client_entry_ptr                                entry,
                           std::string                                              opp_addr,
                           std::string                                              opp_inbound_addr,
                           std::vector<fc::network::ethereum::abi::contract>        abis,
                           uint64_t                                                 outpost_id,
                           uint32_t                                                 chain_id,
                           std::string                                              inbound_read_block_tag = "latest");

   // ── outpost_client SPI ───────────────────────────────────────────────
   sysio::opp::types::ChainKind chain_kind() const override;
   uint64_t                     outpost_id() const override { return _outpost_id; }
   uint32_t                     chain_id()   const override { return _chain_id; }
   // to_string() inherits the base-class default: "{outpost_id}:{ChainKind}:{chain_id}".

   std::string deliver_outbound_envelope(uint32_t                 epoch_index,
                                         const std::vector<char>& envelope_bytes,
                                         fc::microseconds         deadline) override;

   std::vector<char> read_inbound_envelope(uint32_t         epoch_index,
                                           fc::microseconds deadline) override;

   // Expose for inspection / tests
   const ethereum_client_entry_ptr& entry()                  const { return _entry; }
   const std::string&               opp_address()            const { return _opp_addr; }
   const std::string&               opp_inbound_address()    const { return _opp_inbound_addr; }
   const std::string&               inbound_read_block_tag() const { return _inbound_read_block_tag; }

private:
   ethereum_client_entry_ptr                         _entry;
   std::string                                       _opp_addr;
   std::string                                       _opp_inbound_addr;
   std::shared_ptr<opp_contract_client>              _opp_client;
   std::shared_ptr<opp_inbound_contract_client>      _opp_inbound_client;
   uint64_t                                          _outpost_id;
   uint32_t                                          _chain_id;
   std::string                                       _inbound_read_block_tag;
};

using outpost_ethereum_client_ptr = std::shared_ptr<outpost_ethereum_client>;

} // namespace sysio
