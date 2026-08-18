#pragma once

#include <cstddef>

/// OPP outbound-envelope budget.
///
/// These live in `sysio.opp.common` rather than on the `sysio.msgch` contract that enforces
/// them because the budget bounds what any PRODUCER of an attestation may emit, not just the
/// packer that assembles the envelope. `sysio.epoch` derives its OPERATORS-roster ceiling from
/// them, and `sysio.opreg::setconfig` validates the registry ceilings against that — three
/// contracts, one set of numbers. A second copy of a consensus-visible cap is a drift hazard,
/// and the whole point of the cap is that every side agrees on it.
///
/// `sysio.msgch` re-exposes them as members of its contract class so its own packing loop reads
/// them unqualified; those are aliases of these, never copies.
namespace sysio::opp {

/// Hard cap on the encoded envelope size in BOTH directions, mirroring the
/// Solana (`opp_outpost::MAX_ENVELOPE_BYTES`) and Ethereum (`OPP.MAX_ENVELOPE_BYTES`)
/// caps. 32 KiB is the e2e-supported maximum across WIRE / Ethereum / Solana.
/// Solana's 256 KiB BPF heap divided by ~3.3× envelope-size peak heap usage
/// during the finalising chunk's `Envelope::decode + keccak::hash + clone`
/// tolerates more, but Ethereum is the binding constraint: a cold
/// `emitOutboundEnvelope` of a near-64-KiB envelope costs ~45 M gas, ~2.7× the
/// EIP-7825 per-transaction cap of 16 777 216, so the platform cap is 32 768.
/// Outbound, the `buildenv` packing loop uses this to decide how many READY
/// attestations to bundle into the current epoch's envelope; any that don't fit
/// stay in the `attestations` table with status READY for the next epoch's
/// `buildenv` call. Inbound, `deliver` rejects anything larger before hashing
/// or storing it.
inline constexpr size_t MAX_ENVELOPE_BYTES = 32'768;

/// Conservative per-attestation byte budget used by the `buildenv` packing
/// loop: protobuf tags + length prefixes + the attestation type/data-size
/// fields. Over-counts by a few bytes per attestation versus the actual
/// `zpp::bits` encoded size, which keeps the loop O(N) and always errs on the
/// side of leaving a gap. The trailing `packed.size()` check after final
/// serialisation is the hard backstop.
inline constexpr size_t ATTESTATION_OVERHEAD_BYTES = 24;

/// Conservative envelope/message header budget for the packing loop —
/// covers the `Envelope` header fields, the wrapping `Message`, its header
/// + payload preamble, and a safety margin for `zpp::bits` length prefixes.
inline constexpr size_t ENVELOPE_BASELINE_BYTES = 512;

/// Bytes available to a SINGLE attestation that must ride an envelope alone —
/// the envelope cap less the header baseline and that attestation's own framing.
/// This is the figure `buildenv`'s first-attestation-too-big guard rejects against.
inline constexpr size_t SINGLE_ATTESTATION_BUDGET_BYTES =
   MAX_ENVELOPE_BYTES - ENVELOPE_BASELINE_BYTES - ATTESTATION_OVERHEAD_BYTES;

} // namespace sysio::opp
