#include <sysio.cap/sysio.cap.hpp>
#include <sysio.authex/sysio.authex.hpp>

#include <sysio/opp/attestations/attestations.pb.hpp>
#include <zpp_bits.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sysio {

namespace {

   /**
    * @brief Bridge OPP `ChainKind` (the proto enum) to the libfc-lite
    * `chain_kind_t` used by `sysio.authex::links`. The numeric values are
    * intentionally aligned across both definitions (ethereum=2, solana=3,
    * sui=4); the static_cast is the documented bridge — same one
    * `sysio.opreg::regoperator` uses.
    */
   fc::crypto::chain_kind_t to_authex_chain(opp::types::ChainKind k) {
      return static_cast<fc::crypto::chain_kind_t>(k);
   }

   /**
    * @brief Returns true iff `account` has a `sysio.authex::links` row for
    * the given native chain. Used as the gating prerequisite for a wire
    * account to receive WIRE rewards on a position on that chain.
    *
    * Reads the canonical `links_t` table from `sysio.authex` directly.
    * (The readonly-mirror pattern was retired in commit `2d9f0532b1`
    * along with the emissions-readiness gate work; canonical headers are
    * the supported way to do cross-contract reads on this branch.)
    */
   [[maybe_unused]]
   bool authex_link_exists(name account, opp::types::ChainKind chain) {
      sysio::authex::links_t links(cap::AUTHEX_ACCOUNT);
      auto idx = links.get_index<"bynamechain"_n>();
      const uint128_t composite = sysio::to_namechain_key(account, to_authex_chain(chain));
      return idx.find(composite) != idx.end();
   }

   /**
    * @brief Queue an outbound `ATTESTATION_PROCESSING_ERROR` to the source
    * outpost. Used when an inbound staking attestation can't be processed
    * (validation failure, no auth-link, unknown actor, etc.). The outpost
    * is responsible for the user-side refund / undo; wire-sysio never
    * tries to absorb a failed inbound silently.
    *
    * Pattern follows `sysio.epoch::record_gate_block` from commit
    * `2d9f0532b1`, adapted from per-epoch gate blocks to per-attestation
    * processing errors.
    */
   [[maybe_unused]]
   void emit_processing_error(name self,
                              uint64_t outpost_id,
                              uint64_t attestation_id,
                              opp::types::AttestationType original_type,
                              std::vector<char> original_data,
                              std::string_view reason) {
      opp::attestations::AttestationProcessingError msg;
      msg.attestation_id = attestation_id;
      msg.original_type  = original_type;
      msg.original_data  = std::move(original_data);
      msg.error_message  = std::string{reason};

      auto [encoded, out] = zpp::bits::data_out<char>();
      (void)out(msg);

      action(
         permission_level{self, "active"_n},
         cap::MSGCH_ACCOUNT,
         "queueout"_n,
         std::make_tuple(outpost_id,
                         opp::types::AttestationType::ATTESTATION_TYPE_ATTESTATION_PROCESSING_ERROR,
                         encoded)
      ).send();
   }

   /**
    * @brief Send a WIRE transfer from this contract's own account to a
    * recipient. The contract spends from its own balance, which is
    * funded each epoch by `sysio.system::payepoch`'s capital-bucket
    * transfer (see `emissions.cpp:548`).
    */
   [[maybe_unused]]
   void send_wire(name self, name to, int64_t amount, std::string_view memo) {
      if (amount <= 0) return;
      action(
         permission_level{self, "active"_n},
         cap::TOKEN_ACCOUNT,
         "transfer"_n,
         std::make_tuple(self, to, asset{amount, cap::WIRE_SYM}, std::string{memo})
      ).send();
   }

} // anonymous namespace

// ===========================================================================
// setconfig
// ===========================================================================

void cap::setconfig() {
   require_auth(get_self());

   capcfg_t cfgtbl(get_self());
   cap_config cfg = cfgtbl.exists() ? cfgtbl.get() : cap_config{};
   // Concrete config fields land alongside the warmup-fee, pacing, and
   // import-window resolutions in subsequent PRs.
   cfgtbl.set(cfg, get_self());
}

} // namespace sysio
