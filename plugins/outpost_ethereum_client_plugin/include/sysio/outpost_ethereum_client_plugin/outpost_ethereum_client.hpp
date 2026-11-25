#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
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
   /// The outbound envelope answering the epoch has been finalized and
   /// `nextEpochIndex` advanced.
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

/// What the relay should do about the epoch it is holding, given the outpost's
/// view of that epoch.
enum class delivery_action {
   /// Nothing recorded and the epoch has not tipped: deliver the envelope.
   deliver,
   /// This relay's delivery is recorded and the epoch has not tipped. The
   /// outpost tips on the delivery that brings a strict majority of the group
   /// to one digest, so the tip waits on the rest of the group; a re-delivery
   /// is a no-op the contract does not even record.
   await_peers,
   /// The epoch tipped and is not finished, and this relay may carry it on:
   /// its delivery is the settled digest, or it never delivered (a late
   /// member of the group, which the outpost admits with the agreed bytes).
   /// Re-supply the envelope so the outpost dispatches the next stretch.
   continue_dispatch,
   /// The epoch tipped on a digest other than the one this relay delivered: a
   /// minority envelope, which the outpost refuses to resume from, so there is
   /// nothing for this one to send.
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
/// The outpost keeps no time: nothing here depends on when the epoch began
/// or how long the depot's epochs last.
///
/// @param next_epoch_index   the outpost's `nextEpochIndex()`.
/// @param epoch_index        WIRE epoch being delivered.
/// @param spill              the outpost's `dispatchSpill(epoch_index)`.
/// @param settlement         this relay's delivery record, see `classify_settlement`.
delivery_action decide_delivery(uint32_t              next_epoch_index,
                                uint32_t              epoch_index,
                                const dispatch_spill& spill,
                                delivery_settlement   settlement);

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

/// Whether a node's refusal of a read says the block the read was pinned to is
/// not served there -- a load-balanced backend behind the one that confirmed
/// the relay's call, or a node whose state for that block is pruned -- as
/// distinct from a refusal of the request itself (malformed parameters, an
/// unknown method, a revert), which no amount of waiting changes.
///
/// Nodes spell this differently: geth and erigon answer `-32000 header not
/// found` (or `missing trie node` for pruned state), besu `-32000 Block not
/// found`, reth and nethermind the EIP-1474 `-32001` resource-not-found code.
///
/// @param rpc_code  `json_rpc_error::code`.
/// @param message   `json_rpc_error`'s message.
bool is_block_unavailable_refusal(int rpc_code, std::string_view message);

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
   /// `EpochComplete(epoch)`: the call finalized the epoch's outbound envelope
   /// and closed it.
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
/// when no replacement fits the caps and it still pays the market's tip, since it replaces a lower one a
/// node holds. `attempts` must not be empty.
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

/// A point on OPP's outbound hash chain: the chain hash after the `message_end`-th message (exclusive end of the
/// range it closes). The chain is `h_n = keccak256(h_{n-1} || keccak256(message_n))` from `h_0 = 0`.
struct outbound_chain_point {
   /// The chain hash, a normalised word.
   std::string chain_hash;
   /// Messages the chain had folded in at this point.
   uint64_t    message_end = 0;
};

/// Decoded `OPP.getFinalizedOutbound(uint32)`: the envelope the outpost's consensus tip closed for an epoch, and
/// where to find its messages and its publication.
struct finalized_outbound {
   /// Where the envelope ends on the outbound chain.
   outbound_chain_point end;
   /// The envelope's `epochTimestamp`, in milliseconds. Non-zero for every finalized epoch.
   uint64_t             timestamp_ms        = 0;
   /// The block the envelope's first message was emitted in; zero for an envelope with no messages.
   uint64_t             first_message_block = 0;
   /// The block it was published in; zero until it is.
   uint64_t             published_block     = 0;
};

/// Fold `messages`, in order, into OPP's outbound chain from `start`: the chain point the outpost will compare a
/// publication against.
///
/// @param start    where the chain stands before the first message, a normalised word.
/// @param messages the encoded messages, as their `OutboundMessage` events carried them.
/// @return the chain hash after the last message, a normalised word; `start` itself for no messages.
std::string outbound_chain_hash(std::string_view start, const std::vector<std::vector<char>>& messages);

/// One `OutboundMessage` event: a message the outpost emitted, where it sits on the outbound chain, and the
/// block it was emitted in.
struct outbound_message {
   /// Zero-based position in the outbound stream.
   uint64_t          index = 0;
   /// The chain hash after this message, a normalised word.
   std::string       chain_hash;
   /// The encoded message, as the envelope carries it.
   std::vector<char> message;
   /// The block it was emitted in.
   uint64_t          block = 0;
};

/// `OutboundEpochFinalized`: the consensus tip closed an epoch's envelope over messages
/// `[first_message, end.message_end)`.
struct finalized_epoch_event {
   uint32_t             epoch_index   = 0;
   uint64_t             first_message = 0;
   /// Where the envelope ends on the outbound chain.
   outbound_chain_point end;
   /// The envelope's `epochTimestamp`, in milliseconds.
   uint64_t             timestamp_ms  = 0;
};

/// What `outbound_message_cache::follow` did with one message.
enum class follow_result {
   /// Appended: the next message in the stream, chaining from the one before it.
   appended,
   /// Already held, byte for byte: logs read twice.
   duplicate,
   /// Before where the cache starts: already published.
   behind,
   /// The cache cannot take it: messages before it are missing, or it disagrees with a message the cache holds
   /// at its index or does not chain from the one before it (a reorg replaced what was followed). The cache has
   /// to be started again from the outpost's state.
   discontinuous
};

/**
 * @brief The outbound messages a relay has followed past the outpost's published cursor, each checked against
 *        the chain hash its own event carries.
 *
 * Holds a contiguous run of messages from a starting chain point, every one chaining from the one before it:
 * `chainHash_n = keccak256(chainHash_{n-1} || keccak256(message_n))`. A reorg that removes followed messages
 * without replacing them goes unseen here; the end hash an epoch's envelope must reach catches it at use
 * (`between`), and the cache is then started again. Pure: the client feeds it logs and asks it for an epoch's
 * messages.
 */
class outbound_message_cache {
public:
   /// Empty the cache and start it at `start`: the next message it takes is `start.message_end`.
   void reset(outbound_chain_point start);
   /// Empty the cache and leave it unstarted, so the next follow starts it again from the outpost's state.
   void clear();
   /// Whether `reset` started it since the last `clear`.
   bool started() const { return _started; }
   /// The stream position of the first message it holds or will take: where it started, or forgot up to.
   uint64_t begin() const { return _start.message_end; }
   /// The stream position after the last message held.
   uint64_t end() const { return _start.message_end + _messages.size(); }
   /// Whether it holds no message.
   bool empty() const { return _messages.empty(); }

   /// Take one followed message. An unstarted cache takes nothing and reports `discontinuous`.
   follow_result follow(outbound_message message);

   /// The messages `[start.message_end, end.message_end)`, when the cache holds every one of them, the chain
   /// hash before the first is `start.chain_hash`, and the last ends at `end.chain_hash`; `std::nullopt`
   /// otherwise.
   std::optional<std::vector<std::vector<char>>> between(const outbound_chain_point& start,
                                                         const outbound_chain_point& end) const;

   /// Drop the messages before `index` (published): the cache then starts at the chain point after message
   /// `index - 1`. Nothing happens for an index outside what the cache holds.
   void forget_before(uint64_t index);

private:
   outbound_chain_point        _start;
   std::vector<outbound_message> _messages;
   bool                        _started = false;
};

/**
 * @brief Whether `envelope` is the envelope the outpost publishes for `epoch_index`, judged against the settled
 *        record of its finalization.
 *
 * Its header must carry the epoch and the timestamp the outpost finalized it with, and its messages must be
 * exactly those that chain from `start` to the finalized end. Every publication of a finalized epoch carries the
 * same bytes (the header's previous-envelope hash is itself fixed by the earlier epochs), so one that matches is
 * the envelope, whichever block it was read from.
 *
 * @param envelope    The `OPPEnvelope` log's bytes.
 * @param epoch_index The epoch it must be.
 * @param finalized   `getFinalizedOutbound(epoch_index)` at the read confirmation.
 * @param start       Where the epoch's messages start on the outbound chain, at the read confirmation.
 * @return Whether it matches; an envelope that does not decode does not.
 */
bool envelope_matches_finalized(const std::vector<char>& envelope, uint32_t epoch_index,
                                const finalized_outbound& finalized, const outbound_chain_point& start);

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
   /// The relay whose call finalized the epoch publishes the outbound envelope
   /// that call closed, once the finalization is as deep as the read
   /// confirmation: the envelope is fixed then, and no reorg the relay is
   /// configured to survive can turn the publication into a revert. The job
   /// calls again every tick until the depot holds the envelope, so a later
   /// tick publishes what an earlier one could not. A finalization that settles
   /// elsewhere than this relay's call put it, or a publication that fails, is
   /// logged and left to the read path; neither fails the delivery.
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

   /// The envelope the outpost published for `epoch_index`, available once the outpost's record finalizing the
   /// epoch is as deep as the client's read confirmation (`read_settled_block`).
   ///
   /// That record fixes the envelope: its end on the outbound chain, its message count and its timestamp. Any
   /// publication of the epoch carries the same bytes, so the publication need not be settled itself. A
   /// settled one is read from the `OPPEnvelope` log of the block the record names; otherwise one at the head is
   /// read from its block and delivered once it matches the settled record
   /// (`envelope_matches_finalized`).
   ///
   /// Publishing is part of reading. While the epoch is finalized but unpublished at the head, this relay
   /// publishes it, and any unpublished epoch before it first: the followed messages (or, missing those, the
   /// `OutboundMessage` logs from the envelope's first-message block on), re-chained here against the finalized
   /// end before anything is paid for, are handed to `OPP.publishOutboundEnvelope` through the signer's journal.
   /// The call then returns empty and the next poll reads the publication. It publishes nothing while a delivery
   /// holds the signer. Anyone may publish, and the outpost refuses every list but the finalized one, so relays
   /// racing to publish cost at most a no-op.
   ///
   /// @return The envelope bytes, or empty while the epoch's finalization is not settled, no publication of it
   ///         is readable yet, the one read does not match, or its record has left the outpost's retention
   ///         window.
   /// @throws fc::exception on transport failure, deadline expiry, or a publication the outpost refuses.
   std::vector<char> read_inbound_envelope(uint32_t         epoch_index,
                                           fc::microseconds deadline) override;

   /// `OPP.outboundEnvelopes(epoch)` at the client's read confirmation (`read_settled_block`): the checksum the
   /// outpost recorded when it published that epoch's envelope.
   std::optional<fc::sha256> read_emitted_envelope_digest(uint32_t         epoch_index,
                                                          fc::microseconds deadline) override;

   /// The Ethereum crank: `SyndicationPool.realizeYield()` on the pool the
   /// outpost registers as its `DESYNDICATE_LIQ` handler. Idle, at debug level,
   /// while the ABI set carries no `realizeYield` (a deployment that predates
   /// the pool) or no handler is registered; quiet when the pool has nothing to
   /// report. Any other revert and any transport failure propagate.
   void crank_outpost(uint32_t epoch_index, fc::microseconds deadline) override;

   /// Follow the outpost's `OutboundMessage` events from a block cursor up to the head, into a cache checked
   /// message by message against the chain hash each event carries.
   ///
   /// The first call, and the first after the cache broke, starts it from the outpost's state at the head: the
   /// published cursor, and the block of the first message after it (the first unpublished finalized envelope,
   /// early-closed envelope or open envelope that holds one), so every message of a pending epoch is in hand
   /// after a restart. Logs are read in ranges of at most `MAX_LOG_SCAN_BLOCKS` blocks. A message that does not
   /// follow from what the cache holds (a reorg, or a node that skipped a log) clears the cache, and the next
   /// call starts it again. Messages the outpost's published cursor has passed are forgotten, whoever published
   /// them, so the cache holds only what is pending. Idle without an OPP address.
   ///
   /// @throws fc::exception on transport failure or deadline expiry; the cache keeps what it held.
   void follow_outpost(fc::microseconds deadline) override;

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
   /// handled only then, so a delivery a reorg removes is delivered again.
   /// The job retries after the boundary until inbound delivery reaches the depot.
   bool settled_past_delivery(uint32_t epoch_index);

   /// Settle the signer's journal before anything else is sent, the journal's
   /// lock held:
   ///  - forget every submission whose nonce is used at the read confirmation;
   ///  - check every other one against the head, so a call a reorg put back in
   ///    the mempool is pending again;
   ///  - replace a pending one at its nonce when the market priced it out,
   ///    over every signing on record; when no replacement fits the caps, send
   ///    its highest signing again, or, when that signing no longer pays the
   ///    market's tip, a replacement over only what the node holds; send it
   ///    again when the node no longer holds it;
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
   /// when its wait failed (the receipt wait ran out, the send's outcome is
   /// unknown, or the client refused the receipt), which is reported as
   /// `chain::outpost_delivery_incomplete_exception`; a node refusal propagates.
   epoch_in_receipt send_epoch_in(uint32_t epoch_index, const std::string& envelope_hex, uint64_t gas_budget,
                                  fc::time_point deadline_abs);

   /// `OPP.getFinalizedOutbound(epoch_index)` at `block`, or `std::nullopt` when the outpost holds no finalized
   /// record for it: not finalized yet, or pruned past its retention window.
   /// @throws fc::exception on transport failure or an unparsable response.
   std::optional<outpost_ethereum_client_detail::finalized_outbound>
   read_finalized_outbound(uint32_t epoch_index, const fc::network::ethereum::block_number_or_tag_t& block);

   /// The envelope for `epoch_index` among the `OPPEnvelope` logs of `published_block`, or empty when none of
   /// them decodes as that epoch's envelope under the platform cap.
   /// @throws fc::exception on transport failure.
   std::vector<char> read_published_envelope(uint32_t epoch_index, uint64_t published_block);

   /// `OPP.publishedOutboundCursor()` at `block`: where the latest published envelope ended on the outbound chain.
   /// @throws fc::exception on transport failure or an unparsable response.
   outpost_ethereum_client_detail::outbound_chain_point
   read_published_cursor(const fc::network::ethereum::block_number_or_tag_t& block);

   /// The `uint` one argument-less OPP view returns at `block`.
   /// @throws fc::exception on transport failure or an unparsable response.
   uint64_t read_opp_uint(const ethereum_contract_call_fn<fc::variant>& view, const char* view_name,
                          const fc::network::ethereum::block_number_or_tag_t& block);

   /// Every `OutboundMessage` log of blocks `from_block` through `to_block`, in stream order, read in ranges of
   /// at most `MAX_LOG_SCAN_BLOCKS` blocks.
   /// @throws fc::exception on transport failure or a log that does not decode.
   std::vector<outpost_ethereum_client_detail::outbound_message> read_outbound_message_logs(uint64_t from_block,
                                                                                         uint64_t to_block);

   /// Start the followed-message cache from the outpost's state at `head`. Caller holds `_follow_mutex`.
   /// @throws fc::exception on transport failure.
   void start_following(uint64_t head);

   /// Feed `messages` to the cache, clearing it at the first that does not follow. Caller holds
   /// `_follow_mutex`.
   /// @return Whether every message followed.
   bool follow_messages(const std::vector<outpost_ethereum_client_detail::outbound_message>& messages);

   /// The messages that chain from `start` to `end`: from the followed cache when it holds them, otherwise from
   /// the `OutboundMessage` logs of blocks `first_message_block` through `to_block` (and the cache, which missed
   /// them, is cleared to start again). `std::nullopt` when neither yields them.
   /// @throws fc::exception on transport failure.
   std::optional<std::vector<std::vector<char>>> epoch_messages(
      const outpost_ethereum_client_detail::outbound_chain_point& start,
      const outpost_ethereum_client_detail::outbound_chain_point& end, uint64_t first_message_block,
      uint64_t to_block);

   /// The `OutboundMessage` and `OutboundEpochFinalized` events OPP left in a receipt.
   /// @throws fc::exception on a log of either event that does not decode.
   std::pair<std::vector<outpost_ethereum_client_detail::outbound_message>,
             std::optional<outpost_ethereum_client_detail::finalized_epoch_event>>
   receipt_outbound_events(const epoch_in_receipt& receipt);

   /// Publish, in order, every epoch the outpost has finalized but not published at `at_block`, through
   /// `epoch_index`. Each publication is re-chained here against the finalized end before it is paid for; the
   /// first epoch whose messages cannot be had, or that is not finalized at `at_block`, ends the run. `known`
   /// stands in for the read of the epoch it names. Caller holds the signer journal's mutex.
   /// @return How many publications were sent.
   /// @throws fc::exception on transport failure or a publication the outpost refuses;
   ///         `chain::outpost_delivery_incomplete_exception` when one was sent without a receipt.
   uint32_t publish_through(uint32_t epoch_index, const fc::uint256& at_block,
                            const std::optional<outpost_ethereum_client_detail::finalized_epoch_event>& known,
                            fc::time_point deadline_abs);

   /// Send one `publishOutboundEnvelope` after settling the journal, and journal it, as `send_epoch_in` does.
   /// Caller holds the signer journal's mutex.
   epoch_in_receipt send_publication(uint32_t epoch_index, const std::vector<std::vector<char>>& messages,
                                     fc::time_point deadline_abs);

   /// After a call whose receipt shows the epoch finalized: follow the messages the call emitted, and note the
   /// finalization for `publish_settled_finalization`. A failure is logged, never thrown. Caller holds the signer
   /// journal's mutex.
   void note_finalizing_call(uint32_t epoch_index, const epoch_in_receipt& receipt);

   /// Publish `epoch_index` if this relay's call finalized it and that finalization is now settled, i.e. the
   /// record at the read confirmation is the one the call's receipt reported. A record not yet settled keeps the
   /// note for a later tick; one that differs (a reorg undid the call) or a publication that fails drops it, and
   /// the read path publishes the epoch. Never throws. Caller holds the signer journal's mutex.
   void publish_settled_finalization(uint32_t epoch_index, fc::time_point deadline_abs);

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
   /// The epochs this relay's calls finalized and has yet to publish, with what each call's receipt reported.
   /// Guarded by the signer journal's mutex, which every delivery holds.
   std::map<uint32_t, outpost_ethereum_client_detail::finalized_epoch_event> _finalized_here;
   /// Serializes the followed-message cache between the follow job and the delivery and read paths.
   std::mutex                                             _follow_mutex;
   /// The outbound messages followed past the published cursor; see `follow_outpost`.
   outpost_ethereum_client_detail::outbound_message_cache _followed;
   /// The next block `follow_outpost` reads, while `_followed` is started.
   uint64_t                                               _follow_next_block = 0;
   /// Whether the last `read_settled_block` fell back to the finalized block,
   /// so the fallback is logged when it starts and ends rather than per read.
   /// The relay and the underwriter read from different threads.
   std::atomic<bool>                                      _finality_fallback{false};
   uint64_t                                               _outpost_id;
   uint32_t                                               _chain_id;
};

using outpost_ethereum_client_ptr = std::shared_ptr<outpost_ethereum_client>;

} // namespace sysio
