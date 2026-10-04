#pragma once
/**
 * @file sysio.synd.hpp
 * @brief sysio.synd: the depot's syndication contract. It owns every rule between an outpost's
 *        SYNDICATE_LIQ / LIQ_YIELD report and the shadow LIQ a holder receives.
 *
 * Intake, on the envelope path. `sysio.msgch` decodes an accepted inbound envelope, binds each
 * syndication and yield report to the proven outpost, checks the user's key family, the amount range
 * and that the token is an active liq token, then sends `onsynd` for each syndication, `onyield` for
 * each yield report and, after the last of them, `closeenv`. All three carry the outpost, the depot
 * epoch and the envelope's canonical digest. Nothing is paid out at intake:
 *   - a syndication is minted into THIS contract's own `sysio.liq` holder row and held as an `items`
 *     row for the syndicating pubkey, linked or not, with a shadow-custody position started at the
 *     live yield index, so held funds earn WIRE from the moment they arrive;
 *   - a yield report is held as an `items` row carrying a number only; nothing is minted;
 *   - both add to the `(outpost, token, epoch)` `envelopes` row, which `closeenv` moves from OPEN to
 *     WAITING, and to the `(outpost, token)` running sums in `ledger`.
 * An envelope that carried no syndication or yield report writes nothing here.
 *
 * The queue. `closeenv` and permissionless `crank(limit)` run three independently bounded phases:
 *   1. `sync(limit)` advances a persistent envelope cursor, including DONE envelopes awaiting finality.
 *      `syncenv` targets one envelope. Terminal outcomes and settlement snapshots are recorded before
 *      acknowledging bond. This phase moves no funds and operates while the cord is pulled.
 *   2. FIFO release advances from `queue_epoch`. BONDED makes an envelope RELEASABLE; HELD pauses it.
 *      Syndications pass through the bucket and fee into linked accounts or parked balances. Yield
 *      reports use mintyield. INVALID burns from the snapshotted settlement; VALID forwards a pending
 *      challenger share. Neither payout path runs while frozen. Empty item sets become DONE.
 *   3. Underwriting advances from each pair's `underwriting_epoch`, past fully bonded or ruled requests,
 *      and issues its oldest WAITING envelope. Shared bond validation protects non-throwing intake;
 *      requestkeep retains outcomes until acknowledged. A queued syncenv records the real assigned ID.
 * Each of phases 2 and 3 has its own `limit` work budget and `4 * limit` examination cap, with separate
 * pair cursors. Outcome consumption therefore does not require a large release budget or a polling
 * deadline. Late settlement discovered after DONE rewinds the release cursor to that envelope.
 *
 * Challenge. Any account may `challenge` an envelope that is REQUESTED, RELEASABLE or DONE while its
 * request is OPEN or BONDED: it pays the request's hold bond plus the pair's `challenge_extra` in the
 * envelope's token (a charge of zero is refused), the extra stays in `feepot`, and this contract, the
 * issuer, holds the request through `sysio.bond::hold` naming the challenger as the beneficiary. The
 * envelope is HELD and releases nothing more until `sysio` rules, and the pair's queue is rewound to
 * it. One challenge per request.
 *
 * An envelope ruled INVALID. The step that first sees the ruling snapshots the forfeit (the bonded
 * amount) and the bounty returned for want of a hold on the envelope. It burns every unreleased
 * syndication item of the envelope with `sysio.liq::burn` (the WIRE those items earned stays in the
 * pool as slack) and drops its yield items, within its budget; once none is left it pulls what
 * `sysio.bond` still owes it with `sysio.bond::claim`, burns out of the forfeit what the envelope
 * already released, puts the rest and the returned bounty into `feepot`, marks the envelope INVALID and
 * moves the queue on. Holders already paid keep what they received; supply is whole again.
 *
 * An envelope that can never be requested (a shadow too coarse to bond) is dropped by `sysio` with
 * `dropenv`: its held syndications are burned, its yield reports dropped, and it is INVALID.
 *
 * The parked hold. Shadow credited to a pubkey with no AuthX link is kept in `parked`, in this
 * contract's own `sysio.liq` holder row, with a shadow-custody position, so a parked wallet keeps
 * earning WIRE. `linkswept` (inline from `sysio.authex::createlink`) and the permissionless `sweep`
 * deliver it: the balance by a `sysio.liq::transfer` from this contract, the banked WIRE out of the
 * token's `yieldpool` by `sysio.liq::creditowed`, as owed yield of the account's own `sysio.liq` row
 * that `sysio.liq::claim` pays. WIRE the pool cannot cover yet stays banked on the parked row, kept at
 * balance 0, for a later delivery.
 *
 * Desyndication. `desyndicate` ticks the pair's desyndication bucket and refuses a quantity above its
 * level (and any desyndication of a pair with no `syndconfig` row), then moves the holder's shadow here.
 * The fee, `quantity * desynd_fee_bps / 10000` floored, stays in `feepot`; the rest is burned with
 * `sysio.liq::burn`, queued as DESYNDICATE_LIQ to the token's outpost with the next request id, and
 * retained in `returns` until governance establishes its external outcome. The bucket drops by the whole quantity.
 * The attestation carries `total_syndicated`, the depot's outstanding shadow of the symbol
 * after the burn (`liq::outstanding_of`), for the outpost's custody check.
 *
 * The solvency check. Each SYNDICATE_LIQ and LIQ_YIELD carries `total_syndicated`, the outpost's live
 * custody balance of the token, read after the syndication was locked or the yield claimed. Once a message
 * is admitted -- its sequence consumed; a dropped or replayed message is never compared, since it may carry
 * a stale, lower total -- `onsynd` and `onyield` compare it with the depot's outstanding shadow of the
 * symbol, `liq::outstanding_of` (supply plus the yield parked in `liqpending`): `onsynd` with the
 * outstanding after its own mint, since the carried custody already holds this syndication, `onyield` with
 * the outstanding as it is, since the report is held and mints nothing yet. `sysio.msgch` sends an
 * envelope's messages as inline actions, which run depth-first, so each message sees the mints of the ones
 * before it. The check is one-sided, `custody >= outstanding`, because every message in flight moves
 * custody up or the outstanding down: a syndication is locked before it is minted, a desyndication burned
 * before it is paid (or while its payout is stored as pending), yield claimed before it is reported; an
 * INVALID envelope is burned while the outpost keeps the custody, a request-keyed `refundreturn` after
 * definitive external cancellation restores supply the outpost still holds, and anyone may send tokens to a custody account. An
 * excess is printed (`EXCESS`) and nothing else. A shortfall writes a `mismatch` row, pulls the
 * `sysio.andon` cord when this contract is a registered puller (`andon::may_pull`) and the cord is clear,
 * prints why when it does not, and the message is then held, minted and added to its envelope exactly as
 * without it. A total is not a slashable fact. Nothing on this path throws. The check reads live balances
 * only. Each pair retains its latest unresolved incident, cleared only by explicit governance reconciliation.
 * No lifetime reporting totals or completed-return archive are retained.
 *
 * Emergency stop. While the `sysio.andon` cord is pulled nothing leaves this contract, and nothing it
 * holds is burned: the queue step still refreshes requests, records outcomes and issues requests, but
 * releases no item, forwards no hold share and burns no INVALID envelope -- it prints that it waits, and
 * the first step after the cord clears does that work. A VALID ruling is recorded with its hold-share
 * snapshot during the freeze, so the forward survives a prune of the request and is paid once after the
 * clear. `linkswept` keeps the parked
 * balance and returns (the user's link stands; `sweep` delivers after the clear); `sweep`, `desyndicate`,
 * `dropenv` and `sweepyield` are refused. Intake, `challenge` and `setconfig` run. A bucket never refills
 * for an epoch during which the cord was pulled (`bucket_row::frozen_mark`), so after a clear its level
 * is its level at the pull plus the refill of the epochs after the clear; challenge windows run on.
 *
 * Launch. `importsynd` replays pre-launch positions inside the epoch-0 bootstrap window, minting
 * straight to a linked account or into `parked`; `importdone` closes the import for good.
 *
 * Never-throw: `onsynd`, `onyield` and `closeenv` run inside `sysio.msgch`'s consensus transaction.
 * They `check` nothing on their input; a message that fails a rule is dropped with a `DROP`
 * diagnostic on the console and consumes no sequence. The one inline action they send, the
 * `sysio.liq::mint` of a held syndication, is sent only after every condition that `mint` checks has
 * been verified here; the queue step `closeenv` runs keeps the same discipline for every action it
 * sends.
 *
 * Privileged (roa::setsyscode): every row bills the `sysio` RAM pool -- there is no signer to bill on
 * the envelope path -- and inline actions carry this contract's own authority without a
 * `sysio.code` grant. Being privileged, the rows `sysio.bond` creates for its requests bill the
 * `sysio` pool too.
 */

#include <sysio/sysio.hpp>
#include <sysio/crypto.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/asset.hpp>
#include <sysio/slug_name.hpp>
#include <sysio/time.hpp>
#include <sysio/opp/types/types.pb.hpp>
#include <sysio.opp.common/shadow_custody_types.hpp>
#include <sysio.liq/sysio.liq.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace sysio {

   class [[sysio::contract("sysio.synd")]] synd : public contract {
   public:
      using contract::contract;

      /// Configuration and ruling authority; council proposals execute as it.
      static constexpr name SYSTEM_ACCOUNT = "sysio"_n;
      /// The message channel: the only signer of the envelope-path actions.
      static constexpr name MSGCH_ACCOUNT  = "sysio.msgch"_n;
      /// The shadow LIQ ledger this contract mints into and holds in.
      static constexpr name LIQ_ACCOUNT    = "sysio.liq"_n;
      /// The chain registry, for an outpost's chain family.
      static constexpr name CHAINS_ACCOUNT = "sysio.chains"_n;
      /// The AuthX link registry: the only signer of `linkswept`, and the source of every link.
      static constexpr name AUTHEX_ACCOUNT = "sysio.authex"_n;
      /// The WIRE token contract.
      static constexpr name TOKEN_ACCOUNT  = "sysio.token"_n;
      /// The underwriting contract each envelope's statement is registered with.
      static constexpr name BOND_ACCOUNT   = "sysio.bond"_n;

      /// Challenge window of a request, in seconds, for a pair with no `syndconfig` row.
      static constexpr uint32_t DEFAULT_WINDOW_SEC   = 10800;
      /// Units of work of the queue step `closeenv` runs inline: small and fixed, so intake stays cheap;
      /// `crank` does the rest.
      static constexpr uint32_t CLOSEENV_QUEUE_LIMIT = 16;

      /// Lifecycle of one `(outpost, token, epoch)` envelope row.
      enum class envelope_state : uint8_t {
         OPEN,         ///< intake in progress: the envelope's messages are still arriving
         WAITING,      ///< closed by `closeenv`; waiting for its underwriting request
         REQUESTED,    ///< its `sysio.bond` request is issued and not yet bonded or ruled
         RELEASABLE,   ///< the request is BONDED, APPROVED or VALID: items may be released
         HELD,         ///< the request is held; nothing releases until `sysio` rules
         INVALID,      ///< the request was ruled INVALID: the envelope is burned and its forfeit pulled
         DONE          ///< every item is released
      };

      /// What an `items` row holds.
      enum class item_kind : uint8_t {
         SYNDICATION,   ///< shadow minted into this contract's holder row for a beneficiary pubkey
         YIELD          ///< a LIQ_YIELD amount: a number only, minted nowhere until released
      };

      /// The terminal outcome of an envelope's `sysio.bond` request, recorded on the envelope row the first
      /// time the queue sees it. Once recorded the request row is never read again: `sysio.bond` may prune
      /// it once its ruling is PRUNE_RETENTION_SEC old and nothing is owed on it.
      enum class request_outcome : uint8_t {
         PENDING,    ///< not seen terminal yet: the request row is read on every step
         APPROVED,   ///< the challenge window passed with no hold
         VALID,      ///< `sysio` ruled the statement true
         INVALID     ///< `sysio` ruled the statement false
      };

      /// Which flow a leaky bucket limits.
      enum class bucket_direction : uint8_t {
         SYNDICATION,     ///< releases of held syndications to their beneficiaries
         DESYNDICATION    ///< desyndications queued back to the outpost
      };

      // -----------------------------------------------------------------------
      //  Intake (sysio.msgch dispatch; never throw)
      // -----------------------------------------------------------------------

      /// SYNDICATE_LIQ of `amount` base units of the liq token `token_code`, by the holder of
      /// `pubkey` (a `chain_kind` key), in the envelope of outpost `chain_code` accepted for depot
      /// epoch `epoch_index` with canonical digest `digest`, at outpost sequence `sequence`. Mints the
      /// amount into this contract's `sysio.liq` holder row and holds it as a new SYNDICATION item,
      /// whether the pubkey has an AuthX link or not. A pubkey that does not fit `chain_kind`, a token
      /// with no shadow or of another outpost, a `chain_kind` that is not the outpost's, an amount
      /// out of range or past the shadow's headroom, an envelope already closed, an amount that would
      /// take the envelope's total, rounded up to the bond increment, past the asset range (so the
      /// envelope could never be underwritten), or a replayed sequence is dropped with a diagnostic,
      /// never an abort. `total_syndicated` is the outpost's live custody balance of the token, read
      /// after this syndication was locked, as the SYNDICATE_LIQ carried it; once the message is admitted it
      /// is compared with the depot's outstanding shadow after this mint (the file comment's solvency
      /// check), and a shortfall is recorded without changing how the message is held. Auth=sysio.msgch.
      [[sysio::action]] void onsynd(sysio::slug_name chain_code, uint32_t epoch_index, checksum256 digest,
                                    uint64_t sequence, opp::types::ChainKind chain_kind, std::vector<char> pubkey,
                                    sysio::slug_name token_code, uint64_t amount, uint64_t total_syndicated);

      /// LIQ_YIELD of `amount` base units of `token_code`, claimed by outpost `chain_code` at its own
      /// epoch `outpost_epoch`, in the envelope identified as for `onsynd`, at outpost sequence
      /// `sequence` (shared with SYNDICATE_LIQ). Holds the amount as a YIELD item; mints nothing. The
      /// same refusals as `onsynd` apply to the token, the amount, the envelope and the sequence, and
      /// a report that would take the envelope's yield total past the shadow's headroom is dropped too.
      /// `total_syndicated` is the outpost's live custody balance of the token, read after the yield
      /// was claimed into custody, as the LIQ_YIELD carried it; once the report is admitted it is compared
      /// with the depot's outstanding shadow as it stands (the file comment's solvency check), and a shortfall
      /// is recorded without changing how the report is held. Auth=sysio.msgch.
      [[sysio::action]] void onyield(sysio::slug_name chain_code, uint32_t epoch_index, checksum256 digest,
                                     uint64_t sequence, uint64_t outpost_epoch, sysio::slug_name token_code,
                                     uint64_t amount, uint64_t total_syndicated);

      /// Close the envelope of outpost `chain_code` for depot epoch `epoch_index` with canonical digest
      /// `digest`: every OPEN envelope row of that outpost and epoch, one per token, becomes WAITING,
      /// then one step of the queue runs within CLOSEENV_QUEUE_LIMIT. Sent by `sysio.msgch` after the
      /// envelope's last message, only when the envelope carried a syndication or yield report; with no
      /// row it writes nothing. Never throws. Auth=sysio.msgch.
      [[sysio::action]] void closeenv(sysio::slug_name chain_code, uint32_t epoch_index, checksum256 digest);

      // -----------------------------------------------------------------------
      //  Configuration and the queue
      // -----------------------------------------------------------------------

      /// Set the rules of outpost `chain_code` and its liq token `token_code`: the syndication and
      /// desyndication fees in basis points (at most `bond::BPS_DENOMINATOR`), the size and per-epoch
      /// refill of each direction's bucket in base units, the challenge window of each request in seconds
      /// (above 0), the bounty posted on each request from collected fees, and what a challenger pays on
      /// top of the hold bond. A pair with no row issues requests with DEFAULT_WINDOW_SEC and no bounty,
      /// releases no syndication and accepts no desyndication: its buckets are unset. Two settings stall
      /// a pair for good, and nothing but a new configuration moves it: no row, or a syndication bucket
      /// of zero burst, releases no syndication of the pair; and a shadow whose precision is below
      /// `bond::BOND_INCREMENT_DECIMALS` can never be underwritten, so every envelope of it waits (only
      /// `dropenv` clears one). Auth=sysio.
      [[sysio::action]] void setconfig(sysio::slug_name chain_code, sysio::slug_name token_code,
                                       uint32_t synd_fee_bps, uint32_t desynd_fee_bps, uint64_t synd_burst,
                                       uint64_t synd_refill, uint64_t desynd_burst, uint64_t desynd_refill,
                                       uint32_t window_sec, uint64_t bounty, uint64_t challenge_extra);

      /// Run bounded outcome synchronization, FIFO release, and underwriting, in that order. Sync
      /// visits at most `limit` envelopes; underwriting and release each have `limit` work units and
      /// `4 * limit` examinations, with independent cursors. Permissionless. Blocked work waits; while
      /// frozen only synchronization and request issuance run. Durable outcomes have no polling deadline.
      [[sysio::action]] void crank(uint32_t limit);

      /// Synchronize one issued envelope's request identity and terminal outcome without releasing,
      /// burning or claiming funds. Permissionless; safe while frozen and independent of FIFO release.
      [[sysio::action]] void syncenv(sysio::slug_name chain_code, sysio::slug_name token_code,
                                   uint32_t epoch_index);

      /// Synchronize at most `limit` envelopes from a persistent cursor, including DONE envelopes
      /// still awaiting a ruling. Permissionless; no recipient code or funds movement.
      [[sysio::action]] void sync(uint32_t limit);

      /// Erase at most `limit` settled prefix envelopes, retaining a replay floor. Stops at the first
      /// unfinished row; DONE before finality, unacknowledged outcomes, items and pending shares stay.
      /// Permissionless, counts every examined envelope, and never moves funds.
      [[sysio::action]] void pruneenv(sysio::slug_name chain_code, sysio::slug_name token_code, uint32_t limit);

      /// Governance attests that custody is reconciled at `reported` base units, which must cover
      /// current outstanding supply. Erases this pair's incident, leaving the global Andon cord alone.
      /// This is a trusted governance statement, not an on-chain measurement of external custody.
      [[sysio::action]] void reconcile(sysio::slug_name chain_code, sysio::slug_name token_code, uint64_t reported);

      /// Governance confirms an outstanding return was externally paid/completed. Erases the pending
      /// obligation without depot credit. External finality is established off-chain; auth=sysio.
      [[sysio::action]] void finishreturn(uint64_t request_id);

      /// Governance attests irreversible external rejection/cancellation: nothing was paid and no
      /// payable external obligation remains. Restore exactly the net burned amount to the original
      /// holder, atomically erase the outstanding row, and reject a duplicate. Auth=sysio. No timeout
      /// or mere queued/pending/duplicate-request observation is sufficient evidence for this action.
      [[sysio::action]] void refundreturn(uint64_t request_id);

      /// Drop the envelope of outpost `chain_code`, token `token_code` and depot epoch `epoch_index`, OPEN or
      /// WAITING with no request issued: burn its SYNDICATION items out of this contract's `sysio.liq` row
      /// with `sysio.liq::burn` (the WIRE they earned stays in the pool as slack), drop its YIELD items,
      /// and mark it INVALID, so the queue moves past it. The one escape hatch for an envelope the queue
      /// can never request (a shadow too coarse to bond, a statement `sysio.bond` refuses). Refused while the
      /// `sysio.andon` cord is pulled. Auth=sysio.
      [[sysio::action]] void dropenv(sysio::slug_name chain_code, sysio::slug_name token_code, uint32_t epoch_index);

      /// Challenge the envelope of outpost `chain_code`, token `token_code` and depot epoch `epoch_index`,
      /// REQUESTED, RELEASABLE or DONE with its request OPEN or BONDED: `challenger` pays the request's hold
      /// bond (`sysio.bond::hold_bond_of` at `sysio.bond`'s `hold_bps`) plus the pair's `challenge_extra` in
      /// the envelope's token; the extra goes to `feepot`, and `sysio.bond::hold` holds the request,
      /// pulling the hold bond back out of this contract and naming `challenger` as the beneficiary of an
      /// INVALID ruling. The envelope becomes HELD, and the pair's `queue_epoch` is rewound to it so the
      /// queue processes the ruling even for an envelope it had finished. A charge of zero (a hold bond
      /// that rounds to zero and no `challenge_extra`) is refused. One challenge per request.
      /// `challenger` may not be this contract or `sysio.bond`. Contract accounts receive refunds
      /// through ledger settlement without recipient execution.
      /// Challenger's authority; any row the challenge creates bills the `sysio` RAM pool.
      [[sysio::action]] void challenge(name challenger, sysio::slug_name chain_code, sysio::slug_name token_code,
                                       uint32_t epoch_index);

      /// Pull the WIRE `sysio.liq` owes this contract's holder row of the shadow `token_code` into the
      /// token's `yieldpool`, so held and parked positions can be paid out of it, and pay `sysio` what the
      /// token's fee pot earned, as far as the pool covers it: the pot is protocol revenue, and so is its
      /// yield. Refused while the `sysio.andon` cord is pulled. Permissionless.
      [[sysio::action]] void sweepyield(sysio::slug_name token_code);

      // -----------------------------------------------------------------------
      //  The parked hold
      // -----------------------------------------------------------------------

      /// Deliver every parked row of the pubkey `account` has linked for `chain_kind`, for a link that
      /// already exists: late arrivals, and the node-owner path that records links without
      /// `createlink`. Refused when `account` has no link for `chain_kind`, and while the `sysio.andon`
      /// cord is pulled. Permissionless.
      [[sysio::action]] void sweep(name account, opp::types::ChainKind chain_kind);

      /// AuthX link completed: deliver parked principal by callback-free LIQ settlement and banked
      /// WIRE by creditowed. A missing account or malformed key returns quietly. Contract accounts,
      /// including this custodian, can receive without executing code. While Andon is pulled, the
      /// link stands and sweep delivers after the clear.
      /// Auth=sysio.authex.
      [[sysio::action]] void linkswept(name account, opp::types::ChainKind chain_kind, std::vector<char> pubkey);

      // -----------------------------------------------------------------------
      //  Desyndication
      // -----------------------------------------------------------------------

      /// Desyndicate `quantity` of `holder`'s shadow: queue DESYNDICATE_LIQ to the symbol's outpost,
      /// paying the pubkey `holder` has linked for that chain. The pair's desyndication bucket is ticked
      /// to the depot's current epoch; a quantity above its level is refused, as is any desyndication of
      /// a pair with no `syndconfig` row. The quantity moves from the holder's own `sysio.liq` row (never
      /// a held or parked balance) to this contract. The fee, `quantity * desynd_fee_bps /
      /// BPS_DENOMINATOR` floored, stays in `feepot`; the net rest is burned with `sysio.liq::burn`, and
      /// is what the attestation, `returns` carry. The bucket drops
      /// by the whole quantity. A fee that rounds to zero burns and queues the whole quantity; a fee of
      /// the whole quantity is refused. The request takes the next id from `syndcounters`. The attestation
      /// carries `total_syndicated`, the depot's outstanding shadow of the symbol after
      /// this burn (`liq::outstanding_of`: supply plus parked yield, net of the burned amount). The burn is
      /// final: an outpost refusal is reconciled by governance through request-keyed `refundreturn`. Refused while
      /// the `sysio.andon` cord is pulled. Holder's authority.
      [[sysio::action]] void desyndicate(name holder, asset quantity);

      // -----------------------------------------------------------------------
      //  Launch ingestion (privileged caller, epoch-0 bootstrap window)
      // -----------------------------------------------------------------------

      /// One pre-launch position: the holder's native pubkey (32-byte Ed25519 on SVM, 33-byte
      /// compressed secp256k1 on EVM) and its shadow amount in subunits, the LCO yield already folded in.
      struct import_credit {
         std::vector<char> pubkey;       ///< the holder's native pubkey
         uint64_t          amount = 0;   ///< shadow subunits; a zero credit is skipped
         SYSLIB_SERIALIZE(import_credit, (pubkey)(amount))
      };

      /// Replay pre-launch positions of `token_code` on `chain_code`: each credit mints to the account
      /// its pubkey has linked, or into this contract's holder row and a parked row. Batched; the same
      /// pubkey across batches sums. Refused outside the bootstrap window and once `importdone` ran.
      /// Requires this contract's authority, and the contract must be privileged.
      [[sysio::action]] void importsynd(sysio::slug_name chain_code, sysio::slug_name token_code,
                                        std::vector<import_credit> credits);

      /// Close the import: every later `importsynd` is refused. Requires this contract's authority, and
      /// the contract must be privileged.
      [[sysio::action]] void importdone();

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      /// Key of `envelopes`: one row per outpost, token and depot epoch.
      struct envelope_key {
         uint64_t chain_code;    ///< the outpost's registry code value
         uint64_t token_code;    ///< the liq token's registry code value
         uint32_t epoch_index;   ///< the depot epoch the envelope was accepted for
         SYSLIB_SERIALIZE(envelope_key, (chain_code)(token_code)(epoch_index))
      };

      /// What one token of one accepted envelope brought in, and where it stands.
      struct [[sysio::table("envelopes")]] envelope_row {
         sysio::slug_name chain_code;         ///< the outpost that sent the envelope
         sysio::slug_name token_code;         ///< the liq token
         uint32_t         epoch_index = 0;    ///< the depot epoch the envelope was accepted for
         checksum256      digest;             ///< the envelope's canonical epoch digest
         uint64_t         synd_total  = 0;    ///< base units of every SYNDICATION item of this row
         uint64_t         yield_total = 0;    ///< base units of every YIELD item of this row
         uint32_t         item_count  = 0;    ///< items added for this row
         envelope_state   state       = envelope_state::OPEN;   ///< lifecycle state
         /// The `sysio.bond` request covering the row: the id it was issued under, re-read through the
         /// request's `bystatement` key on every refresh; 0 until issued.
         uint64_t         request_id  = 0;
         uint64_t         released    = 0;    ///< base units released to beneficiaries so far
         /// Base units burned after an INVALID ruling: the unreleased syndications, then the released
         /// amount out of the forfeit.
         uint64_t         burned      = 0;
         /// The request's terminal outcome once the queue has seen one; PENDING until then.
         request_outcome  outcome     = request_outcome::PENDING;
         /// Base units the request forfeits to this contract, its bonded amount, snapshotted when the queue
         /// first sees it INVALID; 0 otherwise. The burn that completes the envelope uses it, not the
         /// request row, which `sysio.bond` may prune once it is paid.
         uint64_t         forfeit         = 0;
         /// Base units of bounty `sysio.bond` returns to this contract on that INVALID ruling -- the whole
         /// bounty when the request was never held -- snapshotted with `forfeit`; 0 otherwise.
         uint64_t         bounty_returned = 0;
         /// Base units of the hold bond a challenger is owed back on a VALID ruling -- the share no bond
         /// covered, which `sysio.bond` awards this contract, the issuer -- snapshotted with the VALID
         /// outcome; 0 otherwise. The forward pays it from this snapshot, not the request row, which
         /// `sysio.bond` may prune once it is paid.
         uint64_t         hold_share       = 0;
         /// The challenger `hold_share` is forwarded to, snapshotted with it; empty when there is none.
         name             hold_beneficiary;
         /// True from the step that records a VALID outcome with a positive `hold_share` until the step
         /// that forwards it: the first step while the `sysio.andon` cord is clear.
         bool             share_pending    = false;
         SYSLIB_SERIALIZE(envelope_row, (chain_code)(token_code)(epoch_index)(digest)(synd_total)(yield_total)
                          (item_count)(state)(request_id)(released)(burned)(outcome)(forfeit)(bounty_returned)
                          (hold_share)(hold_beneficiary)(share_pending))
      };

      /// Envelope rows by outpost, token and epoch.
      using envelopes_t = kv::table<"envelopes"_n, envelope_key, envelope_row>;

      /// Key of `items`: the envelope row an item belongs to, then its id, so the items of one envelope
      /// are contiguous and, within it, ascending id is arrival order.
      struct item_key {
         uint64_t chain_code;    ///< the outpost's registry code value
         uint64_t token_code;    ///< the liq token's registry code value
         uint32_t epoch_index;   ///< the depot epoch of its envelope
         uint64_t id;            ///< item id, from `syndcounters`
         SYSLIB_SERIALIZE(item_key, (chain_code)(token_code)(epoch_index)(id))
      };

      /// One held syndication or yield report.
      struct [[sysio::table("items")]] item_row {
         uint64_t                       id          = 0;   ///< item id
         sysio::slug_name               chain_code;        ///< the outpost that reported it
         sysio::slug_name               token_code;        ///< the liq token
         uint32_t                       epoch_index = 0;   ///< the depot epoch of its envelope
         item_kind                      kind        = item_kind::SYNDICATION;   ///< syndication or yield
         /// The beneficiary's key family; CHAIN_KIND_UNKNOWN for a YIELD item, which has none.
         opp::types::ChainKind          chain_kind  = opp::types::ChainKind::CHAIN_KIND_UNKNOWN;
         std::vector<char>              pubkey;            ///< the beneficiary's native pubkey; empty for YIELD
         uint64_t                       amount      = 0;   ///< base units reported
         uint64_t                       remaining   = 0;   ///< base units not yet released
         /// Shadow yield of `remaining` for a SYNDICATION item, started at the live index at intake. A
         /// YIELD item holds no shadow and earns nothing: its position is checkpointed at the live index
         /// at intake and never settled again, so it stays at zero owed, and every walk over items
         /// checks `kind` before it touches a position.
         opp::shadow::custody::position position;
         SYSLIB_SERIALIZE(item_row, (id)(chain_code)(token_code)(epoch_index)(kind)(chain_kind)(pubkey)(amount)
                          (remaining)(position))
      };

      /// Items by envelope and id.
      using items_t = kv::table<"items"_n, item_key, item_row>;

      /// Key of `ledger`: one row per outpost and token.
      struct ledger_key {
         uint64_t chain_code;   ///< the outpost's registry code value
         uint64_t token_code;   ///< the liq token's registry code value
         SYSLIB_SERIALIZE(ledger_key, (chain_code)(token_code))
      };

      /// Operational queue positions and replay floor for one outpost/token; no lifetime totals.
      struct [[sysio::table("ledger")]] ledger_row {
         sysio::slug_name chain_code;            ///< the outpost
         sysio::slug_name token_code;            ///< the liq token
         /// The queue's cursor: no envelope of an earlier epoch is left to refresh or release, so the
         /// queue step starts here.
         uint32_t         queue_epoch      = 0;
         uint32_t         underwriting_epoch = 0; ///< oldest envelope still gating issuance, independent of release
         uint64_t retained_from_epoch = 0; ///< epochs below this floor were compacted and cannot reopen
         SYSLIB_SERIALIZE(ledger_row, (chain_code)(token_code)(queue_epoch)(underwriting_epoch)(retained_from_epoch))
      };

      /// Ledger rows by outpost and token.
      using ledger_t = kv::table<"ledger"_n, ledger_key, ledger_row>;

      /// Key of `syndcursors`: one row per outpost.
      struct cursor_key {
         uint64_t chain_code;   ///< the outpost's registry code value
         SYSLIB_SERIALIZE(cursor_key, (chain_code))
      };

      /// Per-outpost replay guard over the sequence SYNDICATE_LIQ and LIQ_YIELD share.
      struct [[sysio::table("syndcursors")]] synd_cursor {
         sysio::slug_name chain_code;             ///< the outpost
         uint64_t         last_sequence = 0;      ///< highest sequence admitted; anything at or below it is a replay
         uint64_t         last_epoch    = 0;      ///< outpost epoch of the last LIQ_YIELD admitted, for forensics
         SYSLIB_SERIALIZE(synd_cursor, (chain_code)(last_sequence)(last_epoch))
      };

      /// Cursors by outpost.
      using syndcursors_t = kv::table<"syndcursors"_n, cursor_key, synd_cursor>;

      /// Id allocation.
      struct [[sysio::table("syndcounters")]] synd_counters {
         uint64_t next_item_id    = 1;   ///< id the next `items` row takes
         uint64_t next_request_id = 1;   ///< DESYNDICATE_LIQ request id; the outpost reads 0 as "no id"
         SYSLIB_SERIALIZE(synd_counters, (next_item_id)(next_request_id))
      };

      /// The counters singleton.
      using syndcounters_t = kv::global<"syndcounters"_n, synd_counters>;

      /// Key of the per-token tables, `yieldpool` and `feepot`.
      struct token_key {
         uint64_t token_code;   ///< the shadow token's registry code value
         SYSLIB_SERIALIZE(token_key, (token_code))
      };

      /// The shadow-custody solvency pool of one shadow token: the WIRE this contract's holder row
      /// earned, pulled from `sysio.liq` and credited out to held and parked positions.
      struct [[sysio::table("yieldpool")]] pool_row {
         sysio::slug_name                 token_code;   ///< registry code of the shadow token
         opp::shadow::custody::yield_pool pool;         ///< WIRE pulled and credited out
         SYSLIB_SERIALIZE(pool_row, (token_code)(pool))
      };

      /// Yield pools by token code.
      using yieldpools_t = kv::table<"yieldpool"_n, token_key, pool_row>;

      /// Key of `parked`: the shadow token, the chain family and the holder's native pubkey.
      struct parked_key {
         uint64_t          token_code;   ///< the liq token's registry code value
         uint64_t          chain_kind;   ///< magic_enum::enum_integer of the ChainKind; the row keeps the enum
         std::vector<char> pubkey;       ///< the holder's native pubkey
         SYSLIB_SERIALIZE(parked_key, (token_code)(chain_kind)(pubkey))
      };

      /// Shadow credited to a pubkey with no AuthX link yet, held in this contract's own `sysio.liq`
      /// holder row. The position accrues exactly as a holder row does, so linking late costs no yield.
      struct [[sysio::table("parked")]] parked_row {
         sysio::slug_name               token_code;                                           ///< the liq token
         opp::types::ChainKind          chain_kind = opp::types::ChainKind::CHAIN_KIND_UNKNOWN;   ///< key family
         std::vector<char>              pubkey;                                               ///< native pubkey
         uint64_t                       balance    = 0;   ///< shadow base units held for the pubkey
         opp::shadow::custody::position position;         ///< the shadow yield of `balance`
         SYSLIB_SERIALIZE(parked_row, (token_code)(chain_kind)(pubkey)(balance)(position))
      };

      /// Parked rows by token, chain family and pubkey.
      using parkeds_t = kv::table<"parked"_n, parked_key, parked_row>;

      /// Stable identity of an outstanding external return. IDs are never reused.
      struct return_key {
         uint64_t request_id;
         SYSLIB_SERIALIZE(return_key, (request_id))
      };

      /// A burned return awaiting an externally established outcome. Contains only what governance
      /// needs to identify the obligation and restore its original holder if it is definitively rejected.
      struct [[sysio::table("returns")]] return_row {
         uint64_t request_id = 0; ///< DESYNDICATE_LIQ identity
         name holder; ///< original depot holder; later wallet linking cannot redirect a refund
         sysio::slug_name chain_code; ///< destination outpost
         sysio::slug_name token_code; ///< burned token
         opp::types::ChainKind chain_kind; ///< destination key family
         std::vector<char> pubkey; ///< destination captured when queued
         uint64_t amount = 0; ///< exact net burned amount; the fee is not refundable here
         SYSLIB_SERIALIZE(return_row, (request_id)(holder)(chain_code)(token_code)(chain_kind)(pubkey)(amount))
      };
      /// Outstanding returns only; completed/refunded rows are erased atomically.
      using returns_t = kv::table<"returns"_n, return_key, return_row>;

      /// Contract-wide state.
      struct [[sysio::table("syndstate")]] synd_state {
         bool     import_complete = false;   ///< set by `importdone`; every later `importsynd` is refused
         /// The shadow symbol (its `symbol_code` value) whose pair the next queue step starts at: the pair
         /// the last step ran out on (budget or examination cap), or the one after it when that pair
         /// moved nothing.
         uint64_t queue_cursor    = 0;
         uint64_t underwriting_cursor = 0; ///< independent pair cursor for request issuance
         envelope_key sync_cursor{}; ///< next envelope inspected by the independent outcome sweep
         SYSLIB_SERIALIZE(synd_state, (import_complete)(queue_cursor)(underwriting_cursor)(sync_cursor))
      };

      /// The state singleton.
      using syndstate_t = kv::global<"syndstate"_n, synd_state>;

      /// Key of `syndconfig`: one row per outpost and token.
      struct config_key {
         uint64_t chain_code;   ///< the outpost's registry code value
         uint64_t token_code;   ///< the liq token's registry code value
         SYSLIB_SERIALIZE(config_key, (chain_code)(token_code))
      };

      /// The rules of one outpost and token, as `setconfig` set them.
      struct [[sysio::table("syndconfig")]] synd_config {
         sysio::slug_name chain_code;             ///< the outpost
         sysio::slug_name token_code;             ///< the liq token
         uint32_t         synd_fee_bps     = 0;   ///< fee on a released syndication, in basis points
         uint32_t         desynd_fee_bps   = 0;   ///< fee on a desyndication, in basis points
         uint64_t         synd_burst       = 0;   ///< size of the syndication bucket, in base units
         uint64_t         synd_refill      = 0;   ///< syndication bucket refill per depot epoch
         uint64_t         desynd_burst     = 0;   ///< size of the desyndication bucket, in base units
         uint64_t         desynd_refill    = 0;   ///< desyndication bucket refill per depot epoch
         uint32_t         window_sec       = DEFAULT_WINDOW_SEC;   ///< challenge window of each request
         uint64_t         bounty           = 0;   ///< bounty posted on each request, paid from `feepot`
         uint64_t         challenge_extra  = 0;   ///< charged to a challenger on top of the hold bond
         SYSLIB_SERIALIZE(synd_config, (chain_code)(token_code)(synd_fee_bps)(desynd_fee_bps)(synd_burst)
                          (synd_refill)(desynd_burst)(desynd_refill)(window_sec)(bounty)(challenge_extra))
      };

      /// Rules by outpost and token.
      using syndconfig_t = kv::table<"syndconfig"_n, config_key, synd_config>;

      /// Key of `buckets`: one row per outpost, token and direction.
      struct bucket_key {
         uint64_t chain_code;   ///< the outpost's registry code value
         uint64_t token_code;   ///< the liq token's registry code value
         uint8_t  direction;    ///< magic_enum::enum_integer of the bucket_direction; the row keeps the enum
         SYSLIB_SERIALIZE(bucket_key, (chain_code)(token_code)(direction))
      };

      /// A leaky bucket: `level` base units may pass now, and it refills lazily by the configured
      /// refill per depot epoch, up to the burst -- but never for an epoch during which the `sysio.andon`
      /// cord was pulled.
      struct [[sysio::table("buckets")]] bucket_row {
         sysio::slug_name chain_code;                                    ///< the outpost
         sysio::slug_name token_code;                                    ///< the liq token
         bucket_direction direction  = bucket_direction::SYNDICATION;    ///< the flow it limits
         uint64_t         level      = 0;   ///< base units that may pass now; never above the burst
         uint32_t         last_epoch = 0;   ///< the depot epoch it was last ticked at
         /// `andon::frozen_epochs_through` at the last tick: the next tick refills only for the epochs
         /// since `last_epoch` that the cord's frozen count has not grown by.
         uint64_t         frozen_mark = 0;
         SYSLIB_SERIALIZE(bucket_row, (chain_code)(token_code)(direction)(level)(last_epoch)(frozen_mark))
      };

      /// Buckets by outpost, token and direction.
      using buckets_t = kv::table<"buckets"_n, bucket_key, bucket_row>;

      /// The fees collected in one shadow token: a sub-balance of this contract's own `sysio.liq`
      /// holder row, with its own shadow-custody position. Request bounties are paid out of it.
      struct [[sysio::table("feepot")]] fee_row {
         sysio::slug_name               token_code;    ///< the liq token
         uint64_t                       balance = 0;   ///< shadow base units collected and not spent
         opp::shadow::custody::position position;      ///< the shadow yield of `balance`
         SYSLIB_SERIALIZE(fee_row, (token_code)(balance)(position))
      };

      /// Fee pots by token code.
      using feepots_t = kv::table<"feepot"_n, token_key, fee_row>;

      /// One unresolved custody incident per outpost/token.
      struct mismatch_key {
         uint64_t chain_code;   ///< the outpost's registry code value
         uint64_t token_code;   ///< the affected token
         SYSLIB_SERIALIZE(mismatch_key, (chain_code)(token_code))
      };

      /// Latest shortfall evidence for an unresolved incident. A healthy report does not implicitly
      /// clear it; governance explicitly reconciles it against current outstanding supply.
      struct [[sysio::table("mismatch")]] mismatch_row {
         sysio::slug_name chain_code;                            ///< the outpost that reported it
         sysio::slug_name token_code;                            ///< the liq token
         uint32_t         epoch_index = 0;                       ///< the depot epoch of its envelope
         uint64_t         sequence    = 0;                       ///< the outpost sequence of the message
         item_kind        kind        = item_kind::SYNDICATION;  ///< a syndication or a yield report
         uint64_t         reported    = 0;   ///< the outpost custody the message carried, in base units
         /// The depot's outstanding shadow it was compared with, in base units: supply plus parked yield,
         /// after this message's own mint for a syndication.
         uint64_t         expected    = 0;
         time_point_sec   at;                ///< when the shortfall was recorded
         SYSLIB_SERIALIZE(mismatch_row, (chain_code)(token_code)(epoch_index)(sequence)(kind)(reported)(expected)
                          (at))
      };

      /// Current custody incidents by outpost and token.
      using mismatches_t = kv::table<"mismatch"_n, mismatch_key, mismatch_row>;

   private:
      /// Admit `sequence` for outpost `chain_code` and advance its cursor, recording `outpost_epoch`
      /// when it is not zero; false, and nothing written, on a replay. Never throws.
      bool admit_sequence(sysio::slug_name chain_code, uint64_t sequence, uint64_t outpost_epoch);

      /// The solvency check of one admitted message on path `path`: `reported`, the outpost custody of the
      /// shadow `st` the message carried, against `expected`, the depot's outstanding shadow of it. At or
      /// above: nothing, but an excess prints `EXCESS`. Below: a `mismatch` row keyed `(chain_code,
      /// token_code)` for the message of `kind` in the envelope of depot epoch `epoch_index`, and a
      /// `sysio.andon::pull` when `andon::may_pull` admits this contract and the cord is clear; otherwise
      /// the reason the cord was not pulled is printed. Never throws; the caller processes the message
      /// as usual either way.
      void check_custody(std::string_view path, const liq::currency_stats& st, sysio::slug_name chain_code,
                         uint32_t epoch_index, uint64_t sequence, item_kind kind, uint64_t reported,
                         uint64_t expected);

      /// Move every parked row of `(chain_kind, pubkey)` to `account`: for each shadow token, pull
      /// what `sysio.liq` owes this contract's row, take the row's banked WIRE out of the token's
      /// `yieldpool`, transfer its balance to `account` and credit that WIRE to `account`'s `sysio.liq`
      /// row with `sysio.liq::creditowed`. The row is erased, or kept at balance 0 with the WIRE the
      /// pool could not cover yet still banked. Throws only on corrupt `sysio.liq` state, through the
      /// checked `custody::pull`.
      void deliver_parked(name account, opp::types::ChainKind chain_kind, const std::vector<char>& pubkey);

      /// Credit `amount` of `token_code`'s shadow, symbol `sym`, to the account `pubkey` has linked on
      /// `chain_kind`, or to its parked row when it has none, and send the `sysio.liq::mint` that
      /// brings it in.
      void credit_by_pubkey(sysio::slug_name token_code, symbol_code sym, opp::types::ChainKind chain_kind,
                            const std::vector<char>& pubkey, uint64_t amount);
   };

} // namespace sysio
