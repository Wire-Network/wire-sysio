#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fc/int256.hpp>
#include <fc/network/ethereum/ethereum_abi.hpp>
#include <fc/network/ethereum/ethereum_client.hpp>

#include <sysio/outpost_client/outpost_client.hpp>
#include <sysio/outpost_ethereum_client_plugin.hpp>

namespace sysio {

namespace outpost_ethereum_client_detail {

/// Decoded `OPPInbound.dispatchSpill(uint32)` view result — where a tipped
/// epoch's processing stands. All-false/zero for an epoch that has not tipped;
/// an unreadable response is an error, never this.
struct dispatch_spill {
   /// Consensus settled the epoch's digest and dispatch began.
   bool     tipped     = false;
   /// Attestations routed so far — also where the next continuation resumes.
   uint16_t dispatched = 0;
   /// Every attestation has been routed.
   bool     complete   = false;
   /// The outbound envelope has been emitted and `nextEpochIndex` advanced.
   bool     finalized  = false;
};

/// How this relay's recorded delivery for an epoch relates to what the
/// outpost settled on.
enum class delivery_settlement {
   /// Nothing is recorded under this relay for the epoch.
   never_delivered,
   /// Recorded, and the epoch has not tipped: the settled digest is not yet
   /// this epoch's, so there is nothing to compare against.
   recorded,
   /// Recorded on a digest other than the one consensus settled: a minority
   /// envelope, which must not be used to resume the epoch.
   divergent,
   /// Recorded on the settled digest: this relay may continue the epoch.
   settled
};

/// Classify this relay's delivery record for an epoch.
///
/// @param own_digest      `epochDeliveries(epoch, self)` as a normalised word
///                        (lower-case hex, no prefix); the zero word records nothing.
/// @param settled_digest  `pendingEpochHash()` normalised the same way.
/// @param tipped          `dispatchSpill(epoch).tipped`; until the tip the
///                        settled digest belongs to an earlier epoch.
/// @throws fc::exception when either word is not exactly 32 bytes of hex: a
///         malformed read must not pass as "never delivered".
delivery_settlement classify_settlement(std::string_view own_digest, std::string_view settled_digest,
                                        bool tipped);

/// Decoded `OPPInbound.pendingConsensusForDigest(bytes32)` view result.
struct pending_consensus {
   /// The epoch the outpost is accepting.
   uint32_t next_epoch         = 0;
   /// Deliveries recorded for the digest in that epoch.
   uint32_t agreeing           = 0;
   /// Members of the group scheduled for that epoch.
   uint32_t group_size         = 0;
   /// `block.timestamp` at which the outpost's current epoch began.
   uint64_t epoch_started_at   = 0;
   /// The outpost's mirror of the depot's `epoch_duration_sec`.
   uint32_t epoch_duration_sec = 0;
};

/// The contract's path-2 predicate (`.claude/rules/opp-consensus.md`): the
/// boundary has elapsed and a strict majority of the group agrees. A
/// re-delivery sent while this is false is a paid no-op; one sent while it is
/// true re-runs the tip and settles the epoch.
///
/// @param consensus  the view, read for the digest this relay delivered.
/// @param now_sec    wall clock, seconds since the epoch.
bool majority_tip_reachable(const pending_consensus& consensus, uint64_t now_sec);

/// What the relay should do about the epoch it is holding, given the outpost's
/// view of that epoch.
enum class delivery_action {
   /// Nothing recorded and the epoch has not tipped: deliver the envelope.
   deliver,
   /// This relay's delivery is recorded, the epoch has not tipped, and the
   /// contract's path-2 predicate holds: re-send the same envelope so the
   /// outpost re-runs the tip (idempotent on a same-digest re-delivery).
   retry_consensus,
   /// This relay's delivery is recorded and the epoch has not tipped; a
   /// re-delivery would change nothing until more of the group delivers (or,
   /// before the outpost's own boundary, until that boundary passes).
   await_peers,
   /// The epoch tipped on THIS relay's digest and is not finished: re-supply
   /// the envelope so the outpost dispatches the next stretch.
   continue_dispatch,
   /// The epoch tipped on a digest this relay did not deliver. Only a relay
   /// whose recorded delivery matches the settled digest may continue it, so
   /// there is nothing for this one to send.
   wait_for_deliverer,
   /// The outpost has already finalized this epoch: nothing to send.
   already_finalized,
   /// The outpost is still on an earlier epoch: a delivery for this one would
   /// be refused as non-sequential, and the epoch must be retried once the
   /// outpost catches up.
   outpost_behind
};

/// Compare two EVM addresses for identity, tolerating an absent `0x` prefix
/// and EIP-55 checksum casing on either side.
bool same_evm_address(std::string_view lhs, std::string_view rhs);

/// Decide the relay's next move for `epoch_index`.
///
/// Pure and side-effect free so the table is unit-testable without an EVM
/// node; the caller performs whichever RPC the verdict names.
///
/// @param next_epoch_index   the outpost's `nextEpochIndex()`.
/// @param epoch_index        WIRE epoch being delivered.
/// @param spill              the outpost's `dispatchSpill(epoch_index)`.
/// @param settlement         this relay's delivery record, see `classify_settlement`.
/// @param majority_reachable `majority_tip_reachable` for the digest this relay
///                           delivered; consulted only for a recorded, untipped epoch.
delivery_action decide_delivery(uint32_t              next_epoch_index,
                                uint32_t              epoch_index,
                                const dispatch_spill& spill,
                                delivery_settlement   settlement,
                                bool                  majority_reachable);

/// Everything the relay reads about one epoch at ONE block, so the pieces
/// cannot disagree with each other.
struct delivery_progress {
   /// `nextEpochIndex()`.
   uint32_t       next_epoch_index = 0;
   /// `dispatchSpill(epoch)`.
   dispatch_spill spill;
   /// `epochDeliveries(epoch, self)`, normalised; the zero word when none.
   std::string    own_digest;
   /// `pendingEpochHash()`, normalised.
   std::string    settled_digest;
};

/// Whether the outpost moved the epoch forward between two reads: the
/// invariant every call the relay pays for must satisfy. A call that leaves
/// every field where it was did nothing, and sending it again at the same
/// budget would do nothing again.
bool advanced(const delivery_progress& before, const delivery_progress& after);

/// `OPPInbound.epochIn` refusals the relay tells apart in the node's revert
/// bytes. Every one is raised at estimate time, before anything is signed.
enum class epoch_in_revert {
   /// `OPP_DispatchUnderfunded(uint256,uint256)`: the budget does not reach
   /// the contract's dispatch floor.
   dispatch_underfunded,
   /// `OPP_HandlerGasExhausted(address,uint16,uint256)`: the first handler of
   /// the call ran out of its bounded gas — the budget is too small for the
   /// attestation at the cursor, or no budget is.
   handler_gas_exhausted,
   /// `OPP_NonSequentialEpoch(uint32,uint32)`: the outpost is on another epoch.
   non_sequential_epoch,
   /// `OPP_OperatorAlreadyDelivered(uint32,address)`: a re-delivery the
   /// contract treats as already counted.
   operator_already_delivered,
   /// `OPP_NotActiveOperator(address)`: this signer is not in the epoch's
   /// group, or is not the settled digest's deliverer for a continuation.
   not_active_operator,
   /// `OPP_DigestMismatch(bytes32,bytes32)`: the envelope supplied for a
   /// continuation is not the one consensus settled on.
   digest_mismatch
};

/// Classify an `epochIn` revert. `std::nullopt` for a protocol error (the node
/// never ran the call) and for any revert that is not one of the refusals
/// above, exactly shaped: an unknown revert must not pass as a known one.
///
/// @param rpc_code     `json_rpc_error::code`.
/// @param revert_data  `json_rpc_error::data`, the node's revert bytes as `0x`-hex.
std::optional<epoch_in_revert> classify_epoch_in_revert(int rpc_code, std::string_view revert_data);

/// What one confirmed `epochIn` did, read from the `OPPInbound` events in its
/// receipt. Absent events mean the call recorded nothing new, which is the
/// contract's benign no-op for a late or redundant call.
struct delivery_receipt_summary {
   /// `EpochDelivery(epoch, self, digest)`: this relay's delivery was recorded.
   bool                    recorded   = false;
   /// `EpochConsensus(epoch, digest, count)`: the call tipped the epoch.
   bool                    tipped     = false;
   /// `EpochDispatchProgressed(epoch, dispatched)`: where the call's dispatch
   /// stopped, when it dispatched at all.
   std::optional<uint16_t> dispatched;
   /// `EpochComplete(epoch)`: the call emitted and finalized the epoch.
   bool                    finalized  = false;
};

/// Summarise the `OPPInbound` events in one receipt's `logs`.
///
/// @param logs              the receipt's `logs` array, as the node returned it.
/// @param inbound_address   the `OPPInbound` the relay delivers to; logs from
///                          any other address (a handler's, a pool's) are ignored.
/// @param epoch_index       the epoch delivered; events for another are ignored.
/// @param operator_address  this relay's signer, for `EpochDelivery`.
delivery_receipt_summary summarize_delivery_receipt(const fc::variants& logs, std::string_view inbound_address,
                                                    uint32_t epoch_index, std::string_view operator_address);

/// `keccak256` of the envelope bytes as a normalised word — the digest the
/// outpost records for a delivery and settles consensus on.
std::string envelope_digest_word(const std::vector<char>& envelope_bytes);

/// Why `SyndicationPool.realizeYield()` refused, read from the node's revert bytes.
enum class realize_yield_refusal {
   /// `WIRE_NoYield()`: the pool balance equals the principal.
   no_yield,
   /// `WIRE_YieldBelowDeadband(uint64 delta, uint64 deadband)`: accrued, but under
   /// the contract's reporting floor.
   below_deadband,
   /// `WIRE_PoolUnderbacked(uint64 balanceDepot, uint64 principal)`: the balance
   /// fell below the principal, a loss the `LIQYield` carrier cannot express.
   underbacked,
   /// `EnforcedPause()`: OpenZeppelin `Pausable`'s refusal. `realizeYield()` is
   /// `whenNotPaused`, so a pool its panic role has frozen refuses every crank
   /// until it is unpaused -- an expected state, not a failed crank.
   paused
};

/// Classify `realizeYield()`'s revert bytes. `std::nullopt` for anything that is
/// not one of the pool's own refusals, exactly shaped (the selector alone, or the
/// selector plus two words): a role error or a foreign implementation must not
/// pass as a quiet no-op. `EnforcedPause()` is one of the pool's refusals: it
/// comes only from `SyndicationPool`'s own pause (`whenNotPaused`), since the
/// OPP endpoint has no pause of its own.
///
/// @param revert_data  `json_rpc_error::data`, the node's revert bytes as `0x`-hex.
std::optional<realize_yield_refusal> classify_realize_yield_revert(std::string_view revert_data);

/// The address in one raw `eth_call` word, as `0x`-hex, or `std::nullopt` when
/// the hex is not exactly one word of hex digits with a zero 12-byte pad.
std::optional<std::string> address_from_word(std::string_view raw_hex);

/// How many attestations `envelope_bytes` carries across its messages, or
/// `std::nullopt` when the bytes do not decode as an OPP envelope. The relay
/// sizes a delivery's gas budget from it; an unreadable envelope is funded to
/// the ceiling and left to the contract to judge.
std::optional<uint32_t> count_envelope_attestations(const std::vector<char>& envelope_bytes);

/// True when `handler_address` names a contract the outpost routes to: neither
/// `address(0)` (nothing registered) nor `ATTESTATION_BLACKHOLE` (governance
/// dropped the type).
bool is_routable_handler(std::string_view handler_address);

/// What the relay does about a journaled transaction still pending at its nonce.
enum class in_flight_step {
   /// It pays enough to be included at today's prices: keep waiting.
   wait,
   /// Its fee cap is below today's base fee, or the tip it can pay at that base
   /// fee is below the market's: sign it again at the same nonce with every fee
   /// raised.
   replace,
   /// The node holds none of its hashes and nothing at its nonce: send it again
   /// at the same nonce.
   rebroadcast
};

/// Decide what to do about a transaction still pending at its nonce.
///
/// @param known_to_node Whether the node still holds a transaction at the nonce.
/// @param paid          The fees of what the node holds there (`highest_fees` of the attempts it holds).
/// @param market        The node's fee suggestion now (`ethereum_client::get_gas_config`).
in_flight_step decide_in_flight(bool known_to_node, const ethereum_transaction_fees& paid,
                                const ethereum_client::gas_config_t& market);

/// The most one transaction may pay per gas under the transaction policy.
struct fee_caps {
   fc::uint256 max_priority_fee_per_gas;
   fc::uint256 max_fee_per_gas;
};

/// `policy`'s per-gas caps for `transaction`, with the fee cap also bounded by what `max_total_native_cost`
/// leaves after the transaction's value for its gas limit (the policy refuses `gas_limit * max_fee_per_gas +
/// value` above it), and the tip by the fee cap. Fees within these caps are never refused at signing.
fee_caps transaction_fee_caps(const ethereum_transaction_policy& policy, const eip1559_tx& transaction);

/// The highest of each fee across `attempts`. A node holds at most one of them at the nonce, but which one
/// can be unknown (a send whose response was lost), and a replacement priced off the highest is valid against
/// any of them. Zero fees for no attempts.
ethereum_transaction_fees highest_fees(const std::vector<submission_attempt>& attempts);

/// The signing in `attempts` with the highest fee cap, the higher tip breaking a tie: the one sent again
/// when no replacement fits the caps, since it replaces a lower one a node holds. `attempts` must not be
/// empty.
const submission_attempt& highest_signing(const std::vector<submission_attempt>& attempts);

/// The fees a same-nonce resend pays. A replacement must raise each of `paid`'s
/// by more than the 10% nodes require of a replacement; a rebroadcast must pay
/// `paid`. Either pays the market's fees where they are higher, lowered to
/// `caps` where the market goes past them. `nullopt` when `step` is `wait`, or
/// when what it must pay does not fit `caps`.
///
/// @param paid The highest fees any attempt at the nonce was signed with (`highest_fees`).
/// @param caps `transaction_fee_caps` of the transaction resent.
std::optional<ethereum_transaction_fees> resend_fees(in_flight_step step, const ethereum_transaction_fees& paid,
                                                     const ethereum_client::gas_config_t& market,
                                                     const fee_caps&                      caps);

/// The fees to sign at a nonce something with unknown fees holds (a transaction left from before a restart,
/// or another sender's): a tip and a fee cap both of the market's fee cap raised by an eighth, lowered to
/// `caps` where that goes past them. A transaction stuck because the base fee rose past its fee cap, or
/// because the tip it can pay fell under the market's, has a fee cap below the market's, so these raise both
/// of its fees by more than the 10% a replacement needs.
///
/// @param caps `transaction_fee_caps` of the transaction signed.
ethereum_transaction_fees outbid_fees(const ethereum_client::gas_config_t& market, const fee_caps& caps);

/// The lowest nonce from `latest` on that no submission holds, while the node has a pending transaction there
/// that this process did not sign, or a submission waits above it with nothing to fill it, so none of the
/// relay's can be included. `nullopt` otherwise.
///
/// @param latest      The signer's transaction count at the head.
/// @param pending     The signer's transaction count including the node's pending transactions.
/// @param submissions The journal's submissions, ascending by nonce.
std::optional<fc::uint256> first_unaccounted_nonce(const fc::uint256& latest, const fc::uint256& pending,
                                                   const std::vector<ethereum_submission>& submissions);

} // namespace outpost_ethereum_client_detail

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
   /// @throws chain::plugin_config_exception when an OPPInbound address is
   ///         given and the client's static gas ceiling is below
   ///         `DELIVERY_MINIMUM_GAS_CEILING`: no envelope at the platform cap
   ///         could complete on it.
   outpost_ethereum_client(ethereum_client_entry_ptr                                entry,
                           std::string                                              opp_addr,
                           std::string                                              opp_inbound_addr,
                           std::vector<fc::network::ethereum::abi::contract>        abis,
                           uint64_t                                                 chain_code,
                           uint32_t                                                 chain_id);

   // ── outpost_client SPI ───────────────────────────────────────────────
   sysio::opp::types::ChainKind chain_kind() const override;
   uint64_t                     chain_code() const override { return _outpost_id; }
   uint32_t                     chain_id()   const override { return _chain_id; }
   std::vector<uint8_t>         authenticated_caller_address() const override;
   // to_string() inherits the base-class default: "{chain_code}:{ChainKind}:{chain_id}".

   /// Deliver the envelope in ONE `epochIn`, then continue it while the
   /// outpost reports it tipped on this relay's digest and unfinished. Every
   /// decision is taken from a read pinned to one block — the head at the
   /// start of the tick, then the block each confirmed call landed in — and
   /// every call paid for must move the outpost's cursor, or the next one is
   /// funded higher; a call at the ceiling that moves nothing is reported.
   ///
   /// @return The last transaction hash sent, or EMPTY when nothing was sent
   ///         because the outpost had already finalized the epoch or there is
   ///         nothing for this relay to send (the epoch tipped on another
   ///         digest, or this relay's delivery is recorded and awaits peers).
   /// @throws chain::outpost_delivery_incomplete_exception when the tick ends
   ///         with the epoch still open for THIS relay to act on: the outpost
   ///         is behind, the per-tick continuation bound was reached, or a
   ///         call at the ceiling advanced nothing. The job retries next tick.
   /// @throws fc::exception on transport failure or deadline expiry.
   std::string deliver_outbound_envelope(uint32_t                 epoch_index,
                                         const std::vector<char>& envelope_bytes,
                                         fc::microseconds         deadline) override;

   /// `OPP.getLatestOutboundEnvelope()` at the client's read confirmation (`read_settled_block`): the envelope the
   /// outpost emitted for `epoch_index`, or empty while the slot holds another epoch.
   std::vector<char> read_inbound_envelope(uint32_t         epoch_index,
                                           fc::microseconds deadline) override;

   /// `OPP.outboundEnvelopes(epoch)` at the client's read confirmation (`read_settled_block`): the checksum the
   /// outpost recorded when it emitted that epoch's envelope. Throws when the loaded OPP ABI does not declare the
   /// view.
   std::optional<fc::sha256> read_emitted_envelope_digest(uint32_t         epoch_index,
                                                          fc::microseconds deadline) override;

   /// The Ethereum crank: `SyndicationPool.realizeYield()` on the pool the
   /// outpost registers as its `DESYNDICATE_LIQ` handler. Idle, at debug level,
   /// while the ABI set carries no `realizeYield` (a deployment that predates
   /// the pool) or no handler is registered; quiet when the pool has nothing to
   /// report. Any other revert and any transport failure propagate.
   void crank_outpost(uint32_t epoch_index, fc::microseconds deadline) override;

   // Expose for inspection / tests
   const ethereum_client_entry_ptr& entry()                       const { return _entry; }
   const std::string&               opp_address()                 const { return _opp_addr; }
   const std::string&               opp_inbound_address()         const { return _opp_inbound_addr; }
   /// This relay's own signer address in `0x`-hex — the identity the outpost
   /// records deliveries under. Derived once at construction.
   const std::string&               signer_address_hex()          const { return _signer_address_hex; }
   /// The liq syndication pool the crank drives, `0x`-hex, or empty until the
   /// outpost's `DESYNDICATE_LIQ` handler has been discovered.
   const std::string&               syndication_pool_address()    const { return _syndication_pool_addr; }
   /// A copy of the signer's journal: every transaction the relay signed whose nonce is not yet used at the
   /// read confirmation, ascending by nonce.
   std::vector<ethereum_submission> submissions() const;
   /// Bind the pool wrapper the crank drives to `address`, replacing whatever was
   /// bound. Discovery goes through this; tests bind a wrapper whose
   /// `realize_yield` is a stub.
   void bind_syndication_pool(std::string address, std::shared_ptr<syndication_pool_contract_client> client);

private:
   /// The `DESYNDICATE_LIQ` handler the outpost routes to, read from
   /// `OPPInbound.attestationHandlers` at `latest` (the routing table is
   /// configuration, not delivered content), or `std::nullopt` when none is
   /// registered or the word did not decode.
   ///
   /// @throws fc::exception on transport failure.
   std::optional<std::string> discover_syndication_pool();

   /// Read everything `deliver_outbound_envelope` decides from — the epoch
   /// cursor, the spill cursor, this relay's delivery record and the settled
   /// digest — at ONE block.
   ///
   /// Pinned to a block number, never `latest`: the block is the head read at
   /// the start of the tick, or the block a call this tick confirmed in, so a
   /// backend lagging behind it cannot hand back the pre-call state and have
   /// the relay pay for the call twice. A backend that does not know the block
   /// yet is retried for a bounded time.
   ///
   /// Not at the read confirmation either (unlike `read_inbound_envelope`):
   /// the cursor is this relay's own work in progress, and a continuation
   /// gated on confirmation depth would wait it out between every stretch of
   /// dispatch. A reorg under the read costs a paid re-delivery before the tip
   /// or a refused continuation after it; a send missed because the read was
   /// behind is covered by the per-call progress invariant and the next tick.
   ///
   /// @throws fc::exception on transport failure, deadline expiry, or an
   ///         unparsable response.
   outpost_ethereum_client_detail::delivery_progress
   read_progress(uint32_t epoch_index, const fc::network::ethereum::block_number_or_tag_t& block);

   /// Whether the outpost's state at the read confirmation (`read_settled_block`)
   /// already shows this relay's part of `epoch_index` done: the epoch closed or
   /// tipped, or this relay's delivery recorded. The relay reports an epoch
   /// handled only then, so a delivery a reorg removes is delivered again
   /// instead of left to the one consensus retry.
   bool settled_past_delivery(uint32_t epoch_index);

   /// Settle the signer's journal before anything else is sent, the journal's
   /// lock held:
   ///  - forget every submission whose nonce is used at the read confirmation;
   ///  - check every other one against the head, so a call a reorg put back in
   ///    the mempool is pending again;
   ///  - replace a pending one at its nonce when the market priced it out,
   ///    over every signing on record, or send its highest signing again when
   ///    no replacement fits the caps; send it again when the node no longer
   ///    holds it;
   ///  - wait on, and after `unaccounted_nonce_grace` cancel, a nonce no
   ///    submission accounts for: a pending transaction there, or nothing
   ///    below a journaled one;
   ///  - then wait until nothing is pending or `deadline_abs`.
   ///
   /// @return The highest block a journaled submission is in, or `std::nullopt`.
   /// @throws chain::outpost_delivery_incomplete_exception while anything is
   ///         still pending at the deadline.
   std::optional<fc::uint256> settle_submissions(fc::time_point deadline_abs);

   /// Classify `submission` against the head: `landed` when one of its hashes
   /// has a receipt, `superseded` when its nonce is below `latest_count`
   /// without one, `pending` otherwise. Logs each change of state.
   void refresh_submission(ethereum_submission& submission, const fc::uint256& latest_count);

   /// Send `submission` again at its nonce with `fees`: the recorded bytes of its
   /// signing at those fees, or else a new signing, recorded before the send. A
   /// refused send is logged and its attempt kept; the next tick decides again.
   ///
   /// @throws ethereum_transaction_policy_exception when the policy refuses `fees`.
   /// @throws fc::exception on transport failure; the attempt stays recorded.
   void resubmit(ethereum_submission& submission, const ethereum_transaction_fees& fees, std::string_view why);

   /// Send one `epochIn` at `gas_budget`, after settling the journal. The call is
   /// journaled once it was sent: landed when its receipt came back, pending
   /// when its wait failed (the receipt wait ran out, or the send's outcome is
   /// unknown), which is reported as
   /// `chain::outpost_delivery_incomplete_exception`; a node refusal propagates.
   epoch_in_receipt send_epoch_in(uint32_t epoch_index, const std::string& envelope_hex, uint64_t gas_budget,
                                  fc::time_point deadline_abs);

   /// Read `OPPInbound.pendingConsensusForDigest(digest)` at `block` and decode it.
   /// @throws fc::exception on transport failure, deadline expiry, or an
   ///         unparsable response.
   outpost_ethereum_client_detail::pending_consensus read_pending_consensus(const std::string& digest_word,
                                                                            const fc::uint256& block);

   /// The block the reads of what the outpost emitted are made at, from the
   /// client's `read_confirmation`. Across a reorg deeper than the shallower of
   /// two operators' settings the two read different envelopes, and the depot
   /// slashes whichever delivered the one the group's majority did not; an
   /// operator reading stronger than its group also delivers after consensus.
   /// Logs once when finality lag forces the fallback to the finalized block,
   /// and once when it ends.
   ///
   /// @throws fc::exception on transport failure or a node without a head or
   ///         finalized block.
   fc::network::ethereum::block_number_or_tag_t read_settled_block();

   ethereum_client_entry_ptr                              _entry;
   std::string                                            _opp_addr;
   std::string                                            _opp_inbound_addr;

   std::shared_ptr<opp_contract_client>                   _opp_client;
   std::shared_ptr<opp_inbound_contract_client>           _opp_inbound_client;
     // nullable
   /// The plugin's loaded ABI set, kept for the wrapper bound after construction.
   std::vector<fc::network::ethereum::abi::contract>      _abis;
   /// See `syndication_pool_address()` / `bind_syndication_pool`.
   std::string                                            _syndication_pool_addr;
   std::shared_ptr<syndication_pool_contract_client>      _syndication_pool_client;  // nullable
   /// Cached `0x`-hex signer address — see `signer_address_hex()`.
   std::string                                            _signer_address_hex;
   /// The (epoch, cursor) a ceiling-funded call last failed to advance, so the
   /// error is reported once and repeats stay at debug level until the cursor
   /// moves.
   std::optional<std::pair<uint32_t, uint16_t>>           _reported_stall;
   /// Whether the last `read_settled_block` fell back to the finalized block,
   /// so the fallback is logged when it starts and ends rather than per read.
   /// The relay and the underwriter read from different threads.
   std::atomic<bool>                                      _finality_fallback{false};
   uint64_t                                               _outpost_id;
   uint32_t                                               _chain_id;
};

using outpost_ethereum_client_ptr = std::shared_ptr<outpost_ethereum_client>;

} // namespace sysio
