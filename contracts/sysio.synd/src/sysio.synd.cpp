#include <sysio.synd/sysio.synd.hpp>
#include <sysio.andon/sysio.andon.hpp>

#include <limits>

#include <sysio/action.hpp>
#include <sysio/print.hpp>
#include <sysio/privileged.hpp>
#include <sysio/opp/attestations/attestations.pb.hpp>
#include <sysio.authex/sysio.authex.hpp>
#include <sysio.bond/sysio.bond.hpp>
#include <sysio.chains/sysio.chains.hpp>
#include <sysio.epoch/sysio.epoch.hpp>
#include <sysio.liq/sysio.liq.hpp>
#include <sysio.opp.common/depot_native_token.hpp>
#include <sysio.opp.common/envelope_statement.hpp>
#include <sysio.opp.common/require_privileged.hpp>
#include <sysio.opp.common/safe_ops.hpp>
#include <sysio.opp.common/shadow_custody.hpp>
#include <zpp_bits.h>

#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

namespace sysio {

namespace {

using opp::types::AttestationType;
using opp::types::ChainKind;

namespace custody = opp::shadow::custody;

/// The payer of every row this contract writes: the `sysio` RAM pool (privileged-contract model, as
/// sysio.liq uses). The envelope path has no signer whose RAM a held item could draw on, and this
/// contract account has no RAM quota of its own beyond what its code and abi take.
constexpr name ram_payer = synd::SYSTEM_ACCOUNT;

/// Permission this contract sends its own inline actions under.
constexpr name active_permission = "active"_n;

/// `sysio.liq`'s mint, burn and creditowed, which this contract alone may call, and its transfer.
constexpr name mint_action       = "mint"_n;
constexpr name burn_action       = "burn"_n;
constexpr name creditowed_action = "creditowed"_n;
constexpr name transfer_action   = "transfer"_n;
/// `sysio.msgch`'s outbound queue.
constexpr name queueout_action = "queueout"_n;
/// `sysio.bond::request`, which a WAITING envelope sends, and `sysio.liq::mintyield`, which a
/// released yield item sends.
constexpr name request_action   = "request"_n;
constexpr name mintyield_action = "mintyield"_n;
/// `sysio.bond::hold`, which a challenge sends, and `sysio.bond::claim`, which pulls what a ruling awards
/// this contract as the issuer.
constexpr name hold_action  = "hold"_n;
constexpr name claim_action = "claim"_n;
/// `sysio.andon::pull`, which a custody shortfall sends when this contract is a registered puller.
constexpr name pull_action = "pull"_n;

/// Largest amount an `asset` carries, as the unsigned base units every row holds.
constexpr uint64_t max_asset_amount = static_cast<uint64_t>(asset::max_amount);

/// Memos of the transfers a parked delivery and a desyndication send.
constexpr std::string_view parked_delivery_memo = "sysio.synd parked delivery";
constexpr std::string_view desyndicate_memo     = "sysio.synd desyndicate";
/// Memo of the transfers a release to a linked account sends.
constexpr std::string_view release_memo         = "sysio.synd release";
/// Memo of the transfer that pulls a challenger's charge, and of the one that forwards a challenger
/// the share of the hold bond a VALID ruling returns.
constexpr std::string_view challenge_memo       = "sysio.synd challenge";
constexpr std::string_view hold_share_memo      = "sysio.synd hold share";
/// Memo of the transfer that pays `sysio` the WIRE the fee pot earned.
constexpr std::string_view fee_yield_memo       = "sysio.synd fee pot yield";

/// Refusals of the signed actions.
constexpr std::string_view no_link_message       = "account has no link for this chain";
constexpr std::string_view holder_unlinked_msg   = "holder is not AuthX-linked for the token's chain";
constexpr std::string_view not_outpost_message   = "chain_code is not an active outpost";
constexpr std::string_view quantity_message      = "quantity must be positive";
constexpr std::string_view precision_message     = "symbol precision mismatch";
constexpr std::string_view no_symbol_message     = "shadow symbol does not exist";
constexpr std::string_view window_message        = "importsynd is bootstrap-window only";
constexpr std::string_view finalized_message     = "import already finalized";
constexpr std::string_view fee_bps_message       = "fee must be at most 10000 basis points";
constexpr std::string_view window_sec_message    = "window_sec must be positive";
constexpr std::string_view config_range_message  = "bucket, bounty and challenge amounts must fit an asset";
constexpr std::string_view challenger_role_msg   = "the challenger cannot be sysio.synd or sysio.bond";
constexpr std::string_view challenger_code_msg   = "a challenger may not have contract code";
constexpr std::string_view no_envelope_message   = "envelope not found";
constexpr std::string_view challenged_message    = "the envelope's request is already challenged";
constexpr std::string_view unchallengeable_msg   =
   "only a REQUESTED, RELEASABLE or DONE envelope can be challenged";
constexpr std::string_view zero_charge_message   = "challenge charge is zero; configure challenge_extra";
constexpr std::string_view dropenv_state_message =
   "only an OPEN or WAITING envelope with no request issued can be dropped";
constexpr std::string_view no_request_message    = "the envelope's request is not in sysio.bond";
constexpr std::string_view request_final_message =
   "the envelope's request is approved or ruled; it cannot be challenged";
constexpr std::string_view charge_range_message  = "hold bond plus challenge extra exceeds the asset range";
constexpr std::string_view desynd_unset_message  = "desyndication budget is not configured";
constexpr std::string_view desynd_budget_message = "desyndication exceeds the current budget";
constexpr std::string_view desynd_net_message    = "desyndication net of the fee must be positive";

/// Diagnostic path names of the intake actions.
constexpr std::string_view onsynd_path   = "onsynd";
constexpr std::string_view onyield_path  = "onyield";
constexpr std::string_view closeenv_path = "closeenv";

/// Drop reasons, one per rule an intake message can fail.
constexpr std::string_view pubkey_misfit_reason   = "pubkey does not fit the chain family";
constexpr std::string_view no_shadow_reason       = "token_code has no shadow symbol";
constexpr std::string_view other_chain_reason     = "token_code belongs to another chain";
constexpr std::string_view chain_kind_reason      = "chain_kind is not the outpost's chain family";
constexpr std::string_view amount_range_reason    = "amount out of range";
constexpr std::string_view headroom_reason        = "supply exceeds the asset range";
constexpr std::string_view envelope_range_reason  =
   "the envelope's total, rounded up to the bond increment, would exceed the asset range";
constexpr std::string_view envelope_yield_reason  = "the envelope's yield would exceed the shadow's headroom";
constexpr std::string_view envelope_closed_reason = "envelope already closed or of another digest";
constexpr std::string_view replay_reason          = "replayed sequence";
constexpr std::string_view digest_reason          = "envelope digest does not match; left OPEN";

/// Why one step of the queue left an envelope or an item where it was.
constexpr std::string_view no_bond_reason         = "sysio.bond is not deployed; the envelope stays WAITING";
constexpr std::string_view bond_token_reason      = "sysio.bond cannot bond the token; the envelope stays WAITING";
constexpr std::string_view covered_range_reason   = "covered exceeds the asset range; the envelope stays WAITING";
constexpr std::string_view statement_len_reason   = "statement too long for sysio.bond; the envelope stays WAITING";
constexpr std::string_view duplicate_reason       = "statement already requested; the envelope stays WAITING";
constexpr std::string_view missing_request_reason = "request row not found in sysio.bond; the pair waits";
constexpr std::string_view unknown_state_reason   = "request in a state the queue does not know; the pair waits";
constexpr std::string_view dropped_reason         =
   "dropped by sysio: its items are burned and the envelope is INVALID";
constexpr std::string_view invalid_burning_reason =
   "request ruled INVALID; burning the envelope's items continues next step, the pair waits";
constexpr std::string_view invalid_burned_reason  = "request ruled INVALID; the envelope is burned and INVALID";
constexpr std::string_view contract_holder_reason =
   "the challenger has contract code; its share of the hold bond goes to the fee pot";
constexpr std::string_view unset_bucket_reason =
   "no syndconfig row: the syndication bucket is unset, nothing releases";
constexpr std::string_view empty_bucket_reason    = "syndication bucket is empty until it refills";
constexpr std::string_view yield_headroom_reason  = "yield exceeds the shadow's headroom; the item waits";
constexpr std::string_view frozen_share_reason    =
   "the challenger's hold share waits for the andon cord to clear";
constexpr std::string_view frozen_burn_reason     =
   "request ruled INVALID while the andon cord is pulled; the burn waits for the clear, the pair waits";

/// What a queue step prints once while the `sysio.andon` cord is pulled.
constexpr std::string_view frozen_step_note =
   "sysio.synd::queue: the andon cord is pulled; releases, deliveries and burns wait for the clear\n";
/// The solvency check's console verdicts and the reason a shortfall's pull carries. The pull reason is
/// `custody shortfall <chain> <token> seq <n>`, far below `andon::MAX_TEXT_BYTES`.
constexpr std::string_view excess_verdict       = ": EXCESS -- reported ";
constexpr std::string_view shortfall_verdict    = ": SHORTFALL -- reported ";
constexpr std::string_view outstanding_label    = " outstanding ";
constexpr std::string_view shortfall_reason     = "custody shortfall ";
constexpr std::string_view sequence_label       = " seq ";
constexpr std::string_view reason_separator     = " ";
constexpr std::string_view cord_pulled_note     = ": CORD PULLED -- ";
constexpr std::string_view cord_already_note    = ": CORD ALREADY PULLED -- the shortfall is recorded";
constexpr std::string_view cord_not_pulled_note =
   ": CORD NOT PULLED -- sysio.synd is not a registered puller of sysio.andon, or sysio.andon is not deployed";

/// What `linkswept` prints when the cord keeps it from delivering.
constexpr std::string_view frozen_linkswept_note =
   "sysio.synd::linkswept: the andon cord is pulled; the parked balance waits for sweep after the clear\n";

/// Record that message `path` broke `reason` and was dropped. Prints, never throws.
void drop(std::string_view path, std::string_view reason) {
   sysio::print("sysio.synd::", std::string(path), ": DROP -- ", std::string(reason), "\n");
}

/// The chain family of `chain_code` when it is an active outpost, else `std::nullopt`. Never throws.
std::optional<ChainKind> outpost_kind(sysio::slug_name chain_code) {
   return sysio::chains::outpost_kind_of(synd::CHAINS_ACCOUNT, chain_code);
}

/// The checks every intake message shares: `token_code` has a shadow symbol on `sysio.liq`, it
/// mirrors a token of `chain_code`, and `amount` is positive, within the depot amount range and
/// within the shadow's headroom. Returns the shadow's `stat` row, or `std::nullopt` after a drop
/// diagnostic. Touches no row, so a dropped message consumes no sequence. Never throws.
std::optional<liq::currency_stats> resolve_intake(std::string_view path, sysio::slug_name chain_code,
                                                  sysio::slug_name token_code, uint64_t amount) {
   const auto st = liq::find_stat_by_token(synd::LIQ_ACCOUNT, token_code);
   if (!st) {
      drop(path, no_shadow_reason);
      return std::nullopt;
   }
   if (st->chain_code != chain_code) {
      drop(path, other_chain_reason);
      return std::nullopt;
   }
   if (amount == 0 || amount > static_cast<uint64_t>(opp::safe::depot_amount_max)) {
      drop(path, amount_range_reason);
      return std::nullopt;
   }
   if (amount > liq::headroom_of(synd::LIQ_ACCOUNT, *st)) {
      drop(path, headroom_reason);
      return std::nullopt;
   }
   return st;
}

/// The key of the envelope row of `(chain_code, token_code, epoch_index)`.
synd::envelope_key envelope_key_of(sysio::slug_name chain_code, sysio::slug_name token_code, uint32_t epoch_index) {
   return synd::envelope_key{.chain_code = chain_code.value, .token_code = token_code.value,
                             .epoch_index = epoch_index};
}

/// True iff a message of the envelope `(chain_code, epoch_index, digest)` may still add to the row of
/// `token_code`: the row does not exist yet, or it is OPEN with the same digest. Never throws.
bool envelope_accepts(name self, sysio::slug_name chain_code, sysio::slug_name token_code, uint32_t epoch_index,
                      const checksum256& digest) {
   synd::envelopes_t envelopes(self);
   const auto        row = envelopes.try_get(envelope_key_of(chain_code, token_code, epoch_index));
   return !row || (row->state == synd::envelope_state::OPEN && row->digest == digest);
}

/// `total` rounded up to a multiple of `increment`, or `std::nullopt` past the asset range.
std::optional<uint64_t> round_up_to(uint128_t total, uint64_t increment) {
   const uint128_t rounded = (total + increment - 1) / increment * increment;
   if (rounded > max_asset_amount) return std::nullopt;
   return static_cast<uint64_t>(rounded);
}

/// True iff the envelope row of `(chain_code, token_code, epoch_index)` can take one more item of
/// `synd_amount` syndicated and `yield_amount` yield base units of the shadow `st` and stay underwritable:
///   - its syndicated plus yield total, rounded up to the token's bond increment (`bond::increment_of`;
///     a token too coarse to bond is bounded unrounded), fits an `asset`, so `sysio.bond` can be asked
///     to cover it;
///   - for a yield report, its yield total fits the shadow's headroom now (`liq::headroom_of`), so the
///     yield is releasable unless the supply grows meanwhile (a release that finds it grown waits).
/// A message that fails is dropped with a diagnostic under `path`. Reads only; never throws.
bool envelope_fits(std::string_view path, name self, const liq::currency_stats& st, sysio::slug_name chain_code,
                   uint32_t epoch_index, uint64_t synd_amount, uint64_t yield_amount) {
   synd::envelopes_t envelopes(self);
   const auto        row = envelopes.try_get(envelope_key_of(chain_code, st.token_code, epoch_index));
   const uint64_t    synd_total  = row ? row->synd_total : 0;
   const uint64_t    yield_total = row ? row->yield_total : 0;
   const uint128_t   total       = static_cast<uint128_t>(synd_total) + yield_total + synd_amount + yield_amount;
   if (!round_up_to(total, bond::increment_of(st.supply.symbol).value_or(1))) {
      drop(path, envelope_range_reason);
      return false;
   }
   if (yield_amount > 0 &&
       static_cast<uint128_t>(yield_total) + yield_amount > liq::headroom_of(synd::LIQ_ACCOUNT, st)) {
      drop(path, envelope_yield_reason);
      return false;
   }
   return true;
}

/// Add one item of `synd_amount` syndicated and `yield_amount` yield base units to the envelope row of
/// `(chain_code, token_code, epoch_index)`, creating it OPEN with `digest` on the first. Saturating;
/// never throws.
void add_to_envelope(name self, sysio::slug_name chain_code, sysio::slug_name token_code, uint32_t epoch_index,
                     const checksum256& digest, uint64_t synd_amount, uint64_t yield_amount) {
   synd::envelopes_t        envelopes(self);
   const synd::envelope_key key = envelope_key_of(chain_code, token_code, epoch_index);
   synd::envelope_row       row = envelopes.try_get(key).value_or(synd::envelope_row{
      .chain_code  = chain_code,
      .token_code  = token_code,
      .epoch_index = epoch_index,
      .digest      = digest,
   });
   row.synd_total  = opp::safe::add_sat_u64(row.synd_total, synd_amount);
   row.yield_total = opp::safe::add_sat_u64(row.yield_total, yield_amount);
   ++row.item_count;
   envelopes.set(ram_payer, key, row);
}

/// Add `synd_amount`, `yield_amount` and `desynd_amount` base units to the running sums of
/// `(chain_code, token_code)`, creating the row on the first, and return it as stored. The ONE writer
/// of the three sums. Saturating; never throws.
synd::ledger_row add_to_ledger(name self, sysio::slug_name chain_code, sysio::slug_name token_code,
                               uint64_t synd_amount, uint64_t yield_amount, uint64_t desynd_amount) {
   synd::ledger_t         ledger(self);
   const synd::ledger_key key{.chain_code = chain_code.value, .token_code = token_code.value};
   synd::ledger_row       row = ledger.try_get(key).value_or(synd::ledger_row{
      .chain_code = chain_code,
      .token_code = token_code,
   });
   row.syndicated_sum   = opp::safe::add_sat_u64(row.syndicated_sum, synd_amount);
   row.yield_sum        = opp::safe::add_sat_u64(row.yield_sum, yield_amount);
   row.desyndicated_sum = opp::safe::add_sat_u64(row.desyndicated_sum, desynd_amount);
   ledger.set(ram_payer, key, row);
   return row;
}

/// The key of `item` in `items`: its envelope row, then its id.
synd::item_key item_key_of(const synd::item_row& item) {
   return synd::item_key{.chain_code  = item.chain_code.value,
                         .token_code  = item.token_code.value,
                         .epoch_index = item.epoch_index,
                         .id          = item.id};
}

/// Store `item` under the next item id, which it is given. Never throws.
void add_item(name self, synd::item_row item) {
   synd::syndcounters_t counters(self);
   synd::synd_counters  c = counters.get_or_default(synd::synd_counters{});
   item.id                = c.next_item_id++;
   counters.set(c, ram_payer);
   synd::items_t items(self);
   items.set(ram_payer, item_key_of(item), item);
}

/// The key of the parked row of `(token_code, chain_kind, pubkey)`.
synd::parked_key parked_key_of(sysio::slug_name token_code, ChainKind chain_kind, const std::vector<char>& pubkey) {
   return synd::parked_key{.token_code = token_code.value,
                           .chain_kind = static_cast<uint64_t>(magic_enum::enum_integer(chain_kind)),
                           .pubkey     = pubkey};
}

/// The account `pubkey` has linked on `chain_kind`, or an empty name. Never throws.
name linked_account(ChainKind chain_kind, const std::vector<char>& pubkey) {
   const auto pk = public_key_from_op_address(chain_kind, pubkey);
   if (!pk) return name{};   // no link could hold these bytes
   sysio::authex::links_t links(synd::AUTHEX_ACCOUNT);
   auto                   by_pubkey = links.get_index<"bypubkey"_n>();
   const auto             it        = by_pubkey.find(pubkey_to_checksum256(*pk));
   return it == by_pubkey.end() ? name{} : it->username;
}

/// The pubkey `account` has linked on `chain_kind`, or `std::nullopt`. Never throws.
std::optional<std::vector<char>> linked_pubkey(name account, ChainKind chain_kind) {
   sysio::authex::links_t links(synd::AUTHEX_ACCOUNT);
   auto                   by_namechain = links.get_index<"bynamechain"_n>();
   const auto             it           = by_namechain.find(to_namechain_key(account, chain_kind));
   if (it == by_namechain.end()) return std::nullopt;
   return pubkey_to_bytes(it->pub_key);
}

/// `account`'s `active` authority: this contract's own on every inline action it sends, and the
/// holder's on the transfer a desyndication makes on the holder's behalf.
permission_level active_of(name account) { return permission_level{account, active_permission}; }

/// Credit `wire` WIRE this contract holds for `account` -- what a held or parked position banked and
/// the token's pool covered -- to `account`'s `sysio.liq` row of `sym` with `sysio.liq::creditowed`,
/// which moves the WIRE into `sysio.liq` in the same action. The account claims it through
/// `sysio.liq::claim`; no transfer is pushed to it, so an account that would refuse one cannot fail
/// the caller. Nothing for zero.
void credit_owed(name self, name account, symbol_code sym, uint64_t wire) {
   if (wire == 0) return;
   action(active_of(self), synd::LIQ_ACCOUNT, creditowed_action, std::make_tuple(account, sym, wire)).send();
}

/// True iff `account` exists and has contract code. Never throws.
bool has_code(name account) { return is_account(account) && get_code_hash(account) != checksum256{}; }

/// Hold `amount` of the shadow `token_code` (symbol `sym`), already in this contract's own `sysio.liq`
/// row, in the parked row of `(chain_kind, pubkey)`, with `banked` WIRE the pubkey earned elsewhere in
/// this contract added to its position. The position settles at the live index before the balance
/// grows. Nothing is written when both are zero. Never throws.
void park_shadow(name self, sysio::slug_name token_code, symbol_code sym, ChainKind chain_kind,
                 const std::vector<char>& pubkey, uint64_t amount, uint64_t banked) {
   if (amount == 0 && banked == 0) return;
   synd::parkeds_t        parked(self);
   const synd::parked_key key = parked_key_of(token_code, chain_kind, pubkey);
   synd::parked_row       row = parked.try_get(key).value_or(synd::parked_row{
      .token_code = token_code,
      .chain_kind = chain_kind,
      .pubkey     = pubkey,
   });
   custody::settle_and_adjust(row.position, row.balance, static_cast<int64_t>(amount), synd::LIQ_ACCOUNT, sym);
   row.position.owed_wire = std::min(custody::MAX_OWED_WIRE, opp::safe::add_sat_u64(row.position.owed_wire, banked));
   parked.set(ram_payer, key, row);
}

/// Add `amount` of the shadow `token_code` (symbol `sym`) to the token's fee pot, settling its position
/// first. Never throws.
void add_to_feepot(name self, sysio::slug_name token_code, symbol_code sym, uint64_t amount) {
   if (amount == 0) return;
   synd::feepots_t       pots(self);
   const synd::token_key key{token_code.value};
   synd::fee_row         row = pots.try_get(key).value_or(synd::fee_row{.token_code = token_code});
   custody::settle_and_adjust(row.position, row.balance, static_cast<int64_t>(amount), synd::LIQ_ACCOUNT, sym);
   pots.set(ram_payer, key, row);
}

/// Take a bounty of at most `cap` base units of the shadow `token_code` (symbol `sym`) out of its fee
/// pot: what the pot holds when that is less, 0 when it is empty. Returns the bounty. Never throws.
uint64_t take_bounty(name self, sysio::slug_name token_code, symbol_code sym, uint64_t cap) {
   if (cap == 0) return 0;
   synd::feepots_t       pots(self);
   const synd::token_key key{token_code.value};
   auto                  pot = pots.try_get(key);
   if (!pot || pot->balance == 0) return 0;
   const uint64_t bounty = std::min(cap, pot->balance);
   custody::settle_and_adjust(pot->position, pot->balance, -static_cast<int64_t>(bounty), synd::LIQ_ACCOUNT, sym);
   pots.set(same_payer, key, *pot);
   return bounty;
}

/// `amount * bps / bond::BPS_DENOMINATOR` in 128 bits, floored: the fee on `amount`. At most `amount`,
/// because `setconfig` bounds `bps` by the denominator.
uint64_t fee_of(uint64_t amount, uint32_t bps) {
   return static_cast<uint64_t>(static_cast<uint128_t>(amount) * bps / bond::BPS_DENOMINATOR);
}

/// The key of the bucket of `(chain_code, token_code, direction)`.
synd::bucket_key bucket_key_of(sysio::slug_name chain_code, sysio::slug_name token_code,
                               synd::bucket_direction direction) {
   return synd::bucket_key{.chain_code = chain_code.value,
                           .token_code = token_code.value,
                           .direction  = magic_enum::enum_integer(direction)};
}

/// The bucket of `(chain_code, token_code, direction)` ticked to depot epoch `epoch_index`:
/// `level = min(burst, level + refill * epochs refilled)`, in 128 bits, where the epochs refilled are
/// those elapsed since the last tick less those during which the `sysio.andon` cord was pulled (the growth
/// of `andon::frozen_epochs_through` since `frozen_mark`). So a freeze adds no capacity: after the clear
/// the level is the level at the pull plus the refill of the epochs after the clear. An epoch in which the
/// cord was pulled after the bucket had already ticked in it is counted frozen as well, so the rule errs
/// on the side of less capacity, never more. A bucket never ticked starts full. A level above a lowered
/// `burst` is cut to it. Reads only; the caller stores the row. Never throws.
synd::bucket_row tick_bucket(name self, sysio::slug_name chain_code, sysio::slug_name token_code,
                             synd::bucket_direction direction, uint64_t burst, uint64_t refill, uint32_t epoch_index) {
   const uint64_t   frozen = andon::frozen_epochs_through(andon::ANDON_ACCOUNT, epoch_index);
   synd::buckets_t  buckets(self);
   synd::bucket_row row = buckets.try_get(bucket_key_of(chain_code, token_code, direction))
                             .value_or(synd::bucket_row{
                                .chain_code  = chain_code,
                                .token_code  = token_code,
                                .direction   = direction,
                                .level       = burst,
                                .last_epoch  = epoch_index,
                                .frozen_mark = frozen,
                             });
   if (epoch_index > row.last_epoch) {
      const uint64_t elapsed       = epoch_index - row.last_epoch;
      const uint64_t frozen_since  = frozen > row.frozen_mark ? frozen - row.frozen_mark : 0;
      const uint64_t refill_epochs = elapsed > frozen_since ? elapsed - frozen_since : 0;
      const uint128_t refilled = static_cast<uint128_t>(row.level) + static_cast<uint128_t>(refill) * refill_epochs;
      row.level      = refilled < burst ? static_cast<uint64_t>(refilled) : burst;
      row.last_epoch = epoch_index;
   }
   row.frozen_mark = frozen;
   row.level       = std::min(row.level, burst);
   return row;
}

/// True iff the queue step may push a transfer to `account`: an existing account without contract code,
/// other than this contract. A transfer notifies its recipient, and a contract that refused it would
/// make the step, and with it the envelope path, fail for every holder. Never throws.
bool pushable_account(name self, name account) {
   return account != name{} && account != self && is_account(account) && !has_code(account);
}

/// The account a release of `pubkey`'s shadow is pushed to: the one the pubkey has linked on
/// `chain_kind` at release time, when `pushable_account` admits it. An empty name otherwise, and the
/// release goes into `parked`, where the account takes delivery through `sweep` or `linkswept`, in which
/// a refusal fails only its own action. Never throws.
name deliverable_account(name self, ChainKind chain_kind, const std::vector<char>& pubkey) {
   const name account = linked_account(chain_kind, pubkey);
   return pushable_account(self, account) ? account : name{};
}

/// Base units request `request_id`'s escrow of `kind` still awards `self` on `sysio.bond`, to claim: 0
/// when the escrow does not exist, awards another account, or was claimed. Never throws.
uint64_t award_of(name self, uint64_t request_id, bond::escrow_kind kind) {
   bond::escrows_t escrows(synd::BOND_ACCOUNT);
   const auto      row = escrows.try_get(bond::escrow_key_of(request_id, kind));
   return row && row->payee == self ? row->payout : 0;
}

/// The statement `sysio.bond` underwrites for envelope `row`: its outpost, epoch, digest and token.
std::vector<char> statement_of(const synd::envelope_row& row) {
   return opp::envelope_statement::pack(opp::envelope_statement::statement{
      .chain_code  = row.chain_code.value,
      .epoch_index = row.epoch_index,
      .digest      = row.digest,
      .token_code  = row.token_code.value,
   });
}

/// The key `sysio.bond`'s `bystatement` index holds a request by `self` for `statement` under.
checksum256 request_key_of(name self, const std::vector<char>& statement) {
   return bond::statement_key_of(self, bond::statement_digest_of(opp::envelope_statement::schema, statement));
}

/// The `sysio.bond` request `self` issued for envelope `row`, found through its `bystatement` key rather
/// than the stored id; `std::nullopt` when there is none (never issued, or pruned). Never throws.
std::optional<bond::request_row> find_request(name self, const synd::envelope_row& row) {
   bond::requests_t requests(synd::BOND_ACCOUNT);
   const auto       by_statement = requests.get_index<bond::STATEMENT_INDEX>();
   const auto       it           = by_statement.find(request_key_of(self, statement_of(row)));
   if (it == by_statement.end()) return std::nullopt;
   return *it;
}

/// Record that the queue step left `(chain_code, token_code, epoch_index)` where it was, for `reason`.
/// Prints, never throws.
void note(sysio::slug_name chain_code, sysio::slug_name token_code, uint32_t epoch_index, std::string_view reason) {
   sysio::print("sysio.synd::queue: ", chain_code.to_string(), " ", token_code.to_string(), " epoch ", epoch_index,
                " -- ", std::string(reason), "\n");
}

/// One step of the queue (the file header's three parts), bounded by a budget of work units and by an
/// examination cap of EXAMINE_FACTOR times that budget, which bounds the rows it reads whatever the
/// number of pairs. Every
/// outpost and token is walked from its `queue_epoch` in epoch order, which within a pair is item id
/// order, so no pair's backlog stops another's. Never throws of its own; the one check it can reach is
/// `custody::pull`'s, which fails only on corrupt `sysio.liq` state. While the `sysio.andon` cord is
/// pulled the step releases, forwards and burns nothing: it refreshes, records outcomes and issues
/// requests only, so it sends no action that moves funds out of this contract.
class queue_step {
public:
   /// Examinations a step may make per work unit of its budget.
   static constexpr uint32_t EXAMINE_FACTOR = 4;

   /// A step of at most `limit` work units and `EXAMINE_FACTOR * limit` examinations, run by the contract
   /// `self`.
   queue_step(name self, uint32_t limit)
      : self_(self), left_(limit), looks_(static_cast<uint32_t>(std::min<uint64_t>(
                                            uint64_t{EXAMINE_FACTOR} * limit, std::numeric_limits<uint32_t>::max()))),
        epoch_(epoch::current_epoch_index()), frozen_(andon::pulled(andon::ANDON_ACCOUNT)) {}

   /// Run every shadow's pair, in shadow-symbol order from the pair the last step ran out on, wrapping
   /// around, until the budget or the examination cap is spent. Every pair visited costs one examination
   /// for its fixed reads, idle or not, so a step's reads are bounded by the cap whatever the number of
   /// pairs. The next step starts at the pair this step runs out on when that pair released or burned
   /// something, or when the cap ran out before it could look at any of its envelopes, and at the pair
   /// after it otherwise: a pair that keeps moving holds the cursor until its backlog drains, and a pair
   /// that moves nothing never holds it.
   void run() {
      if (frozen_) sysio::print(std::string(frozen_step_note));
      synd::syndstate_t       state(self_);
      synd::synd_state        stored = state.get_or_default(synd::synd_state{});
      const uint64_t          start  = stored.queue_cursor;
      liq::stats              statstable(synd::LIQ_ACCOUNT);
      std::optional<uint64_t> stopped_at;
      bool                    hold          = false;   // the next step starts at the pair it ran out on
      for (auto it = statstable.lower_bound(liq::symbol_key{start}); it != statstable.end() && !spent(); ++it) {
         const bool reached = run_pair(*it);
         if (spent()) {
            stopped_at = it.key().symbol_code;
            hold       = pair_.moved || !reached;
         }
      }
      for (auto it = statstable.begin(); it != statstable.end() && !spent(); ++it) {
         if (it.key().symbol_code >= start) break;   // walked above
         const bool reached = run_pair(*it);
         if (spent()) {
            stopped_at = it.key().symbol_code;
            hold       = pair_.moved || !reached;
         }
      }
      if (!stopped_at) return;
      uint64_t cursor = *stopped_at;
      if (!hold) {
         // The pair after it, or the first pair when it was the last: cursor 0 starts at the first.
         const auto next = statstable.lower_bound(liq::symbol_key{cursor + 1});
         cursor          = next != statstable.end() ? next.key().symbol_code : 0;
      }
      if (cursor != start) {
         stored.queue_cursor = cursor;
         state.set(stored, ram_payer);
      }
   }

private:
   /// What one pair's walk has found so far.
   struct pair_state {
      bool                          synd_stopped   = false;   ///< no further syndication of the pair releases
      bool                          yield_stopped  = false;   ///< no further yield of the pair releases
      bool                          moved          = false;   ///< an item of the pair was released or burned
      uint64_t                      yield_reserved = 0;       ///< headroom taken by this step's yield mints
      std::optional<synd::pool_row> pool;                     ///< the token's yield pool, once pulled
   };

   /// Spend one work unit; false when none is left.
   bool spend() {
      if (left_ == 0) return false;
      --left_;
      return true;
   }

   /// Count one examination -- a pair visited, an envelope looked at, an item row read; false when the
   /// cap is reached.
   /// Separate from the budget, which it never touches.
   bool look() {
      if (looks_ == 0) return false;
      --looks_;
      return true;
   }

   /// The step can do nothing more: its budget or its examination cap is spent.
   bool spent() const { return left_ == 0 || looks_ == 0; }

   /// How many rows a walk may still collect: no more than it can pay for or look at.
   uint32_t collectable() const { return std::min(left_, looks_); }

   /// Walk the envelopes of the pair of `st` from its ledger's `queue_epoch`, at most as many as the
   /// budget and the examination cap leave: refresh each issued request, release RELEASABLE envelopes,
   /// issue the oldest WAITING envelope's request when no earlier one is unbonded and unruled, and move
   /// `queue_epoch` past the finished prefix. Each envelope looked at and each item row read counts one
   /// examination. The refresh reads state only and spends no budget, so every issued request the walk
   /// reaches has its outcome recorded, whatever the pair's flows; releasing, burning and requesting
   /// spend the budget. Entry costs one examination, for the ledger and config rows and the first
   /// envelope lookup. False when the cap ran out before the walk could look at an envelope the pair
   /// has; true otherwise, an idle pair included.
   bool run_pair(const liq::currency_stats& st) {
      pair_ = pair_state{};
      look();   // run() visits a pair only while the cap has room, so this entry look always succeeds
      synd::ledger_t         ledger(self_);
      const synd::ledger_key lkey{.chain_code = st.chain_code.value, .token_code = st.token_code.value};
      auto                   lrow = ledger.try_get(lkey);
      if (!lrow) return true;   // nothing was ever admitted for this pair
      synd::syndconfig_t configs(self_);
      const auto config =
         configs.try_get(synd::config_key{.chain_code = lkey.chain_code, .token_code = lkey.token_code});

      // Collected first: the walk rewrites the rows it visits. Nothing after the first WAITING
      // envelope can move -- the rest is WAITING or still OPEN.
      synd::envelopes_t               envelopes(self_);
      std::vector<synd::envelope_key> keys;
      auto first = envelopes.lower_bound(envelope_key_of(st.chain_code, st.token_code, lrow->queue_epoch));
      const bool has_envelope = first != envelopes.end() && first.key().chain_code == lkey.chain_code &&
                                first.key().token_code == lkey.token_code;
      if (has_envelope && collectable() == 0) return false;   // the cap ran out on the entry look
      for (auto it = first; it != envelopes.end() && keys.size() < collectable(); ++it) {
         const synd::envelope_key& key = it.key();
         if (key.chain_code != lkey.chain_code || key.token_code != lkey.token_code) break;
         keys.push_back(key);
         if (it->state == synd::envelope_state::WAITING) break;
      }

      uint32_t queue_epoch = lrow->queue_epoch;
      bool     finished    = true;    // every envelope walked so far is DONE or INVALID
      bool     unbonded    = false;   // an earlier request is neither bonded nor ruled: nothing moves past it
      for (const auto& key : keys) {
         if (!look()) break;
         auto row = envelopes.try_get(key);
         if (!row) continue;
         const synd::envelope_state  before     = row->state;
         const synd::request_outcome outcome    = row->outcome;
         const uint64_t              request_id = row->request_id;
         const uint64_t              released   = row->released;
         const uint64_t              burned     = row->burned;
         const bool                  pending    = row->share_pending;
         if (row->state == synd::envelope_state::REQUESTED || row->state == synd::envelope_state::RELEASABLE ||
             row->state == synd::envelope_state::HELD) {
            refresh(*row, unbonded, st);
         }
         if (row->state == synd::envelope_state::RELEASABLE && !unbonded && !frozen_) {
            release_items(*row, config, st);
         }
         if (row->state == synd::envelope_state::WAITING && !unbonded && spend()) issue_request(*row, config);
         // The outcome's snapshots (`forfeit`, `bounty_returned`, `hold_share`, `hold_beneficiary`) are only
         // ever written with the outcome; `share_pending` clears on its own when the forward is paid.
         if (row->state != before || row->outcome != outcome || row->request_id != request_id ||
             row->released != released || row->burned != burned || row->share_pending != pending) {
            envelopes.set(ram_payer, key, *row);
         }

         const bool done = row->state == synd::envelope_state::DONE || row->state == synd::envelope_state::INVALID;
         if (finished && done) {
            queue_epoch = key.epoch_index + 1;
         } else {
            finished = false;
         }
      }
      if (queue_epoch != lrow->queue_epoch) {
         lrow->queue_epoch = queue_epoch;
         ledger.set(ram_payer, lkey, *lrow);
      }
      return true;
   }

   /// Refresh `row` from its `sysio.bond` request, found through the request's `bystatement` key. OPEN
   /// keeps it REQUESTED; BONDED makes it RELEASABLE; HELD makes it HELD. The first time the request is
   /// seen APPROVED, VALID or INVALID the outcome is recorded on `row` and the request row is never read
   /// again: APPROVED and VALID make it RELEASABLE (VALID forwarding a challenger its share of the hold
   /// bond), INVALID burns it (`invalidate`). Sets `unbonded` when the request is neither bonded nor
   /// ruled -- OPEN, or HELD before it was fully bonded -- when no request row is found for an envelope
   /// with no recorded outcome, while an INVALID envelope's burn is unfinished, and on any state it
   /// does not know, where the items are left untouched.
   void refresh(synd::envelope_row& row, bool& unbonded, const liq::currency_stats& st) {
      using request_state = bond::request_state;
      using outcome       = synd::request_outcome;
      if (row.outcome == outcome::INVALID) {
         invalidate(row, st, unbonded);
         return;
      }
      if (row.outcome != outcome::PENDING) {
         if (row.share_pending) forward_hold_share(row, st);
         row.state = synd::envelope_state::RELEASABLE;
         return;
      }
      const auto req = find_request(self_, row);
      if (!req) {
         note(row.chain_code, row.token_code, row.epoch_index, missing_request_reason);
         unbonded = true;
         return;
      }
      row.request_id = req->id;
      if (req->state == request_state::OPEN) {
         row.state = synd::envelope_state::REQUESTED;
         unbonded  = true;
      } else if (req->state == request_state::BONDED) {
         row.state = synd::envelope_state::RELEASABLE;
      } else if (req->state == request_state::HELD) {
         row.state = synd::envelope_state::HELD;
         if (req->bonded < req->covered) unbonded = true;
      } else if (req->state == request_state::APPROVED) {
         row.outcome = outcome::APPROVED;
         row.state   = synd::envelope_state::RELEASABLE;
      } else if (req->state == request_state::VALID) {
         // The challenger's share is snapshotted with the outcome, frozen or not, so the forward never
         // depends on the request row, which sysio.bond may prune once it is paid.
         row.outcome = outcome::VALID;
         if (req->hold_bond > 0 && req->hold_beneficiary != name{}) {
            row.hold_share       = bond::share_of(req->covered - req->bonded, req->hold_bond, req->covered);
            row.hold_beneficiary = req->hold_beneficiary;
            row.share_pending    = row.hold_share > 0;
         }
         if (row.share_pending) forward_hold_share(row, st);
         row.state = synd::envelope_state::RELEASABLE;
      } else if (req->state == request_state::INVALID) {
         // The forfeit and the bounty sysio.bond returns are snapshotted now: the burn may span steps,
         // and sysio.bond may prune the row once they are claimed.
         row.outcome         = outcome::INVALID;
         row.forfeit         = req->bonded;
         row.bounty_returned = req->hold_beneficiary == name{} ? req->bounty : 0;
         invalidate(row, st, unbonded);
      } else {
         note(row.chain_code, row.token_code, row.epoch_index, unknown_state_reason);
         unbonded = true;
      }
   }

   /// Forward the challenger the hold share snapshotted on VALID envelope `row` (`hold_share` to
   /// `hold_beneficiary`), once, and clear `share_pending`: the share of the hold bond no bond covered,
   /// which `sysio.bond` awards this contract, the issuer, and which the challenger paid. While the
   /// `sysio.andon` cord is pulled it waits, pending, for the first step after the clear. The award is
   /// claimed first while `sysio.bond` still owes it (a claim with nothing owed would throw; anyone may have
   /// claimed it for this contract already, and a request pruned since was fully paid, so the share sits in
   /// this contract's row either way), then sent by `sysio.liq::transfer`. A challenger that has gained
   /// contract code since could refuse the transfer and fail the step, so its share goes to the fee pot.
   void forward_hold_share(synd::envelope_row& row, const liq::currency_stats& st) {
      if (frozen_) {
         note(row.chain_code, row.token_code, row.epoch_index, frozen_share_reason);
         return;
      }
      pull_first(st);
      if (award_of(self_, row.request_id, bond::escrow_kind::HOLD_BOND) > 0) send_claim(row.request_id);
      if (pushable_account(self_, row.hold_beneficiary)) {
         action(active_of(self_), synd::LIQ_ACCOUNT, transfer_action,
                std::make_tuple(self_, row.hold_beneficiary,
                                asset{static_cast<int64_t>(row.hold_share), st.supply.symbol},
                                std::string{hold_share_memo}))
            .send();
      } else {
         note(row.chain_code, row.token_code, row.epoch_index, contract_holder_reason);
         add_to_feepot(self_, row.token_code, st.supply.symbol.code(), row.hold_share);
      }
      row.share_pending = false;
   }

   /// Burn envelope `row`, whose request was ruled INVALID, from the forfeit and bounty snapshotted on it.
   /// Every item of the envelope is erased in id order, one work unit each: a syndication's remainder is
   /// burned with `sysio.liq::burn` (its position settled; the WIRE it earned stays in the pool as slack),
   /// a yield report is dropped. While items are left the pair waits. Once none is, the forfeit -- every
   /// bond, which `rslvinvalid` records for the issuer -- and the bounty returned when the request was
   /// never held are pulled with `sysio.bond::claim` while sysio.bond still owes them to this contract
   /// (a claim with nothing owed would throw; anyone may have claimed them for this contract already,
   /// and a request row pruned since the snapshot was fully paid, so its tokens sit in this contract's
   /// row). What the envelope released is burned out of the forfeit, and the rest, with the returned
   /// bounty, goes to the fee pot. The envelope is INVALID and the pair moves on. The claimed tokens
   /// arrive by the claim's inline transfer, which runs before the burn sent after it.
   void invalidate(synd::envelope_row& row, const liq::currency_stats& st, bool& unbonded) {
      // Burns and the claim that pulls the forfeit wait for the cord to clear; the outcome and its snapshot
      // are already recorded, so nothing is lost if sysio.bond prunes the request meanwhile.
      if (frozen_) {
         note(row.chain_code, row.token_code, row.epoch_index, frozen_burn_reason);
         unbonded = true;
         return;
      }
      const symbol_code code = st.supply.symbol.code();
      pull_first(st);
      synd::items_t items(self_);
      bool          emptied    = true;
      uint64_t      unreleased = 0;
      for (const auto& key : item_keys(row, emptied)) {
         if (!look() || !spend()) {
            emptied = false;
            break;
         }
         auto item = items.try_get(key);
         if (!item) continue;
         if (item->kind == synd::item_kind::SYNDICATION) {
            const uint64_t remaining = item->remaining;
            custody::settle_and_adjust(item->position, item->remaining, -static_cast<int64_t>(remaining),
                                       synd::LIQ_ACCOUNT, code);
            unreleased = opp::safe::add_sat_u64(unreleased, remaining);
         }
         items.erase(key);
         pair_.moved = true;
      }
      burn(row, unreleased);
      if (!emptied) {
         note(row.chain_code, row.token_code, row.epoch_index, invalid_burning_reason);
         unbonded = true;
         return;
      }

      bond::requests_t requests(synd::BOND_ACCOUNT);
      const auto       req = requests.try_get(bond::request_key{row.request_id});
      if (req && (req->forfeit_pending > 0 || award_of(self_, req->id, bond::escrow_kind::BOUNTY) > 0)) {
         send_claim(req->id);
      }
      const uint64_t repaid = std::min(row.released, row.forfeit);
      burn(row, repaid);
      add_to_feepot(self_, row.token_code, code, opp::safe::add_sat_u64(row.forfeit - repaid, row.bounty_returned));
      row.state = synd::envelope_state::INVALID;
      note(row.chain_code, row.token_code, row.epoch_index, invalid_burned_reason);
   }

   /// Burn `amount` of `row`'s token out of this contract's `sysio.liq` row, which holds it, and count
   /// it in `row.burned`. Nothing for zero.
   void burn(synd::envelope_row& row, uint64_t amount) {
      if (amount == 0) return;
      action(active_of(self_), synd::LIQ_ACCOUNT, burn_action, std::make_tuple(row.token_code, amount)).send();
      row.burned = opp::safe::add_sat_u64(row.burned, amount);
   }

   /// Pull what `sysio.bond` owes this contract, as the issuer, on terminal request `request_id`.
   void send_claim(uint64_t request_id) {
      action(active_of(self_), synd::BOND_ACCOUNT, claim_action, std::make_tuple(request_id, self_)).send();
   }

   /// The id `sysio.bond` will assign to the next request this step sends. Its counter is read once:
   /// the requests this step sends run after it, in order, with nothing between them, so they take
   /// consecutive ids from there.
   uint64_t take_request_id() {
      if (!next_request_id_) {
         next_request_id_ =
            bond::bondcounters_t(synd::BOND_ACCOUNT).get_or_default(bond::bond_counters{}).next_request_id;
      }
      return (*next_request_id_)++;
   }

   /// Ask `sysio.bond` to underwrite WAITING envelope `row`: every condition `request` checks is
   /// verified first, so the inline action cannot fail; on a failure the envelope stays WAITING and the
   /// reason is printed. The request id is the one `sysio.bond` will assign (`take_request_id`).
   void issue_request(synd::envelope_row& row, const std::optional<synd::synd_config>& config) {
      if (!has_code(synd::BOND_ACCOUNT)) {
         note(row.chain_code, row.token_code, row.epoch_index, no_bond_reason);
         return;
      }
      const auto custody_token =
         opp::custody::resolve_depot_native_token(synd::LIQ_ACCOUNT, synd::TOKEN_ACCOUNT, row.token_code);
      const auto increment = custody_token ? bond::increment_of(custody_token->sym) : std::nullopt;
      if (!increment) {
         note(row.chain_code, row.token_code, row.epoch_index, bond_token_reason);
         return;
      }
      const auto covered =
         round_up_to(static_cast<uint128_t>(row.synd_total) + static_cast<uint128_t>(row.yield_total), *increment);
      if (!covered) {
         note(row.chain_code, row.token_code, row.epoch_index, covered_range_reason);
         return;
      }
      std::vector<char> statement = statement_of(row);
      if (statement.size() > bond::MAX_STATEMENT_BYTES) {
         note(row.chain_code, row.token_code, row.epoch_index, statement_len_reason);
         return;
      }
      if (find_request(self_, row)) {
         note(row.chain_code, row.token_code, row.epoch_index, duplicate_reason);
         return;
      }

      const uint32_t window_sec = config ? config->window_sec : synd::DEFAULT_WINDOW_SEC;
      const uint64_t bounty =
         take_bounty(self_, row.token_code, custody_token->sym.code(), config ? config->bounty : uint64_t{0});
      const uint64_t request_id = take_request_id();
      // sysio.bond pulls the bounty from this contract's liq row, where the fee pot's share of it sat.
      action(active_of(self_), synd::BOND_ACCOUNT, request_action,
             std::make_tuple(self_, opp::envelope_statement::schema, std::move(statement), row.token_code, *covered,
                             bounty, window_sec))
         .send();
      row.request_id = request_id;
      row.state      = synd::envelope_state::REQUESTED;
   }

   /// Release the items of RELEASABLE envelope `row` in id order while the budget lasts; the envelope
   /// is DONE once none is left. An item of a flow that has stopped for this step is passed over
   /// without spending: it cannot move until a later step.
   void release_items(synd::envelope_row& row, const std::optional<synd::synd_config>& config,
                      const liq::currency_stats& st) {
      synd::items_t items(self_);
      bool          emptied = true;   // every item of the envelope is gone
      for (const auto& key : item_keys(row, emptied)) {
         if (!look()) {
            emptied = false;
            break;
         }
         auto item = items.try_get(key);
         if (!item) continue;
         // The kind is checked first: a YIELD item holds no shadow and is never settled.
         const bool is_yield = item->kind == synd::item_kind::YIELD;
         if (is_yield ? pair_.yield_stopped : pair_.synd_stopped) {
            emptied = false;
            continue;
         }
         if (!spend()) {
            emptied = false;
            break;
         }
         const uint64_t remaining = item->remaining;
         if (is_yield) {
            release_yield(*item, row, st);
         } else {
            release_syndication(*item, row, config, st);
         }
         if (item->remaining != remaining) pair_.moved = true;
         if (item->remaining == 0) {
            items.erase(key);
         } else {
            emptied = false;
            if (item->remaining != remaining) items.set(ram_payer, key, *item);
         }
      }
      if (emptied) row.state = synd::envelope_state::DONE;
   }

   /// The keys of envelope `row`'s items in id order, at most the budget and examinations left: collected
   /// first, because the walk rewrites or erases the rows it visits. Clears `emptied` when items are left
   /// past them.
   std::vector<synd::item_key> item_keys(const synd::envelope_row& row, bool& emptied) {
      synd::items_t               items(self_);
      const synd::item_key        first{.chain_code  = row.chain_code.value,
                                        .token_code  = row.token_code.value,
                                        .epoch_index = row.epoch_index,
                                        .id          = 0};
      std::vector<synd::item_key> keys;
      for (auto it = items.lower_bound(first); it != items.end(); ++it) {
         const synd::item_key& key = it.key();
         if (key.chain_code != first.chain_code || key.token_code != first.token_code ||
             key.epoch_index != first.epoch_index) {
            break;
         }
         if (keys.size() >= collectable()) {
            emptied = false;
            break;
         }
         keys.push_back(key);
      }
      return keys;
   }

   /// Release one tranche of syndication `item`: tick the pair's bucket, take `min(remaining, level)`,
   /// move its fee to the fee pot and the rest to the account the pubkey has linked now, with the WIRE
   /// the item banked, or into `parked`. Stops the pair's syndications when the bucket is unset or
   /// empty.
   void release_syndication(synd::item_row& item, synd::envelope_row& row,
                            const std::optional<synd::synd_config>& config, const liq::currency_stats& st) {
      if (pair_.synd_stopped) return;
      if (!config) {
         note(item.chain_code, item.token_code, item.epoch_index, unset_bucket_reason);
         pair_.synd_stopped = true;
         return;
      }
      synd::bucket_row bucket = tick_bucket(self_, item.chain_code, item.token_code,
                                            synd::bucket_direction::SYNDICATION, config->synd_burst,
                                            config->synd_refill, epoch_);
      const uint64_t tranche = std::min(item.remaining, bucket.level);
      bucket.level -= tranche;
      synd::buckets_t(self_).set(ram_payer,
                                 bucket_key_of(item.chain_code, item.token_code, synd::bucket_direction::SYNDICATION),
                                 bucket);
      if (tranche == 0) {
         note(item.chain_code, item.token_code, item.epoch_index, empty_bucket_reason);
         pair_.synd_stopped = true;
         return;
      }

      const uint64_t    fee  = fee_of(tranche, config->synd_fee_bps);
      const uint64_t    net  = tranche - fee;
      const symbol      sym  = st.supply.symbol;
      const symbol_code code = sym.code();
      const name        account = deliverable_account(self_, item.chain_kind, item.pubkey);
      if (account != name{}) {
         // Pull first -- nothing this release queues may touch this contract's liq row ahead of the
         // claim (custody obligation 3) -- then take the item's banked WIRE out of what the pool covers.
         synd::pool_row& pool   = pulled_pool(st);
         const uint64_t  banked = custody::settle_and_take(item.position, item.remaining, pool.pool,
                                                           synd::LIQ_ACCOUNT, code);
         synd::yieldpools_t(self_).set(ram_payer, synd::token_key{st.token_code.value}, pool);
         custody::settle_and_adjust(item.position, item.remaining, -static_cast<int64_t>(tranche), synd::LIQ_ACCOUNT,
                                    code);
         // What the pool could not cover stays banked on the item for its next tranche; once the item is
         // released whole it would be erased with it, so it moves to the pubkey's parked row instead,
         // which a later `sweep` pays once the pool covers it.
         if (item.remaining == 0 && item.position.owed_wire > 0) {
            park_shadow(self_, item.token_code, code, item.chain_kind, item.pubkey, 0, item.position.owed_wire);
            item.position.owed_wire = 0;
         }
         if (net > 0) {
            action(active_of(self_), synd::LIQ_ACCOUNT, transfer_action,
                   std::make_tuple(self_, account, asset{static_cast<int64_t>(net), sym}, std::string{release_memo}))
               .send();
         }
         credit_owed(self_, account, code, banked);
      } else {
         // Parked: the shadow stays in this contract's row, and the WIRE the item banked moves with it.
         custody::settle_and_adjust(item.position, item.remaining, -static_cast<int64_t>(tranche), synd::LIQ_ACCOUNT,
                                    code);
         const uint64_t banked   = item.position.owed_wire;
         item.position.owed_wire = 0;
         park_shadow(self_, item.token_code, code, item.chain_kind, item.pubkey, net, banked);
      }
      add_to_feepot(self_, item.token_code, code, fee);
      row.released = opp::safe::add_sat_u64(row.released, tranche);
   }

   /// Release yield `item` whole into `sysio.liq`'s pending yield by `mintyield`: no fee and no
   /// budget. When the shadow's headroom, net of this step's earlier yield, cannot take it, the item
   /// stays and the pair's yields stop until a later step.
   void release_yield(synd::item_row& item, synd::envelope_row& row, const liq::currency_stats& st) {
      if (pair_.yield_stopped) return;
      const uint64_t room = liq::headroom_of(synd::LIQ_ACCOUNT, st);
      const uint64_t free = room > pair_.yield_reserved ? room - pair_.yield_reserved : 0;
      if (item.remaining > free) {
         note(item.chain_code, item.token_code, item.epoch_index, yield_headroom_reason);
         pair_.yield_stopped = true;
         return;
      }
      action(active_of(self_), synd::LIQ_ACCOUNT, mintyield_action,
             std::make_tuple(item.chain_code, item.token_code, item.remaining))
         .send();
      pair_.yield_reserved += item.remaining;
      row.released          = opp::safe::add_sat_u64(row.released, item.remaining);
      item.remaining        = 0;
   }

   /// Pull the pair's yield pool, once per step, and store it: before a path that sends nothing else
   /// to the pool queues an action that touches this contract's liq row of the token (custody
   /// obligation 3).
   void pull_first(const liq::currency_stats& st) {
      synd::yieldpools_t(self_).set(ram_payer, synd::token_key{st.token_code.value}, pulled_pool(st));
   }

   /// The yield pool of the pair's token, pulled once per step before anything the step queues
   /// touches this contract's liq row of the token.
   synd::pool_row& pulled_pool(const liq::currency_stats& st) {
      if (!pair_.pool) {
         synd::pool_row pool = synd::yieldpools_t(self_)
                                  .try_get(synd::token_key{st.token_code.value})
                                  .value_or(synd::pool_row{.token_code = st.token_code});
         custody::pull(pool.pool, synd::LIQ_ACCOUNT, self_, st.supply.symbol.code());
         pair_.pool = pool;
      }
      return *pair_.pool;
   }

   name                    self_;              ///< this contract
   uint32_t                left_;              ///< work units left
   uint32_t                looks_;             ///< examinations left
   uint32_t                epoch_;             ///< the depot epoch the buckets tick to
   bool                    frozen_;            ///< the `sysio.andon` cord is pulled: nothing leaves or burns
   pair_state              pair_;              ///< the pair being walked
   std::optional<uint64_t> next_request_id_;   ///< id of the next request this step sends, once read
};

} // anonymous namespace

// ---------------------------------------------------------------------------
//  Intake
// ---------------------------------------------------------------------------

void synd::onsynd(sysio::slug_name chain_code, uint32_t epoch_index, checksum256 digest, uint64_t sequence,
                  ChainKind chain_kind, std::vector<char> pubkey, sysio::slug_name token_code, uint64_t amount,
                  uint64_t total_syndicated) {
   require_auth(MSGCH_ACCOUNT);
   if (!pubkey_fits(chain_kind, pubkey)) {
      drop(onsynd_path, pubkey_misfit_reason);
      return;
   }
   const auto st = resolve_intake(onsynd_path, chain_code, token_code, amount);
   if (!st) return;
   const auto kind = outpost_kind(chain_code);
   if (!kind || *kind != chain_kind) {
      drop(onsynd_path, chain_kind_reason);
      return;
   }
   if (!envelope_accepts(get_self(), chain_code, token_code, epoch_index, digest)) {
      drop(onsynd_path, envelope_closed_reason);
      return;
   }
   if (!envelope_fits(onsynd_path, get_self(), *st, chain_code, epoch_index, amount, 0)) return;
   // Every check is behind us: the sequence is consumed only by a message that is held.
   if (!admit_sequence(chain_code, sequence, 0)) {
      drop(onsynd_path, replay_reason);
      return;
   }
   // The carried custody already holds this syndication: compare it with the outstanding after its mint.
   // resolve_intake bounded the amount by the headroom, so the sum stays within the asset range.
   check_custody(onsynd_path, *st, chain_code, epoch_index, sequence, item_kind::SYNDICATION, total_syndicated,
                 liq::outstanding_of(LIQ_ACCOUNT, *st) + amount);

   // The position is settled at balance 0 first, so it starts at the live index: the item earns
   // exactly what the shadow minted into this contract's row below earns, from now on.
   item_row item{
      .chain_code  = chain_code,
      .token_code  = token_code,
      .epoch_index = epoch_index,
      .kind        = item_kind::SYNDICATION,
      .chain_kind  = chain_kind,
      .pubkey      = std::move(pubkey),
      .amount      = amount,
   };
   custody::settle_and_adjust(item.position, item.remaining, static_cast<int64_t>(amount), LIQ_ACCOUNT,
                              st->supply.symbol.code());
   add_item(get_self(), std::move(item));
   add_to_envelope(get_self(), chain_code, token_code, epoch_index, digest, amount, 0);
   add_to_ledger(get_self(), chain_code, token_code, amount, 0, 0);

   // resolve_intake verified every condition `mint` checks: the shadow exists and the amount is
   // positive and within its headroom. This contract is an account, so the mint cannot fail.
   action(active_of(get_self()), LIQ_ACCOUNT, mint_action, std::make_tuple(get_self(), token_code, amount)).send();
}

void synd::onyield(sysio::slug_name chain_code, uint32_t epoch_index, checksum256 digest, uint64_t sequence,
                   uint64_t outpost_epoch, sysio::slug_name token_code, uint64_t amount,
                   uint64_t total_syndicated) {
   require_auth(MSGCH_ACCOUNT);
   const auto st = resolve_intake(onyield_path, chain_code, token_code, amount);
   if (!st) return;
   if (!envelope_accepts(get_self(), chain_code, token_code, epoch_index, digest)) {
      drop(onyield_path, envelope_closed_reason);
      return;
   }
   if (!envelope_fits(onyield_path, get_self(), *st, chain_code, epoch_index, 0, amount)) return;
   if (!admit_sequence(chain_code, sequence, outpost_epoch)) {
      drop(onyield_path, replay_reason);
      return;
   }
   // The report is held and mints nothing yet: compare with the outstanding as it stands.
   check_custody(onyield_path, *st, chain_code, epoch_index, sequence, item_kind::YIELD, total_syndicated,
                 liq::outstanding_of(LIQ_ACCOUNT, *st));

   // A number only: no shadow is minted for it. Its position still checkpoints the live index, settled
   // at balance 0, so a walk that settled it by mistake would credit nothing rather than the index's
   // whole history; every item walk checks the kind first all the same.
   item_row item{
      .chain_code  = chain_code,
      .token_code  = token_code,
      .epoch_index = epoch_index,
      .kind        = item_kind::YIELD,
      .amount      = amount,
      .remaining   = amount,
   };
   uint64_t no_shadow = 0;
   custody::settle_and_adjust(item.position, no_shadow, 0, LIQ_ACCOUNT, st->supply.symbol.code());
   add_item(get_self(), std::move(item));
   add_to_envelope(get_self(), chain_code, token_code, epoch_index, digest, 0, amount);
   add_to_ledger(get_self(), chain_code, token_code, 0, amount, 0);
}

void synd::closeenv(sysio::slug_name chain_code, uint32_t epoch_index, checksum256 digest) {
   require_auth(MSGCH_ACCOUNT);
   // One envelope row per token at most: walk the outpost's shadow tokens, a set bounded by the
   // registry, rather than every envelope row the outpost ever produced.
   liq::stats  statstable(LIQ_ACCOUNT);
   envelopes_t envelopes(get_self());
   for (auto it = statstable.begin(); it != statstable.end(); ++it) {
      if (it->chain_code != chain_code) continue;
      const envelope_key key = envelope_key_of(chain_code, it->token_code, epoch_index);
      auto               row = envelopes.try_get(key);
      if (!row || row->state != envelope_state::OPEN) continue;
      if (row->digest != digest) {
         drop(closeenv_path, digest_reason);
         continue;
      }
      row->state = envelope_state::WAITING;
      envelopes.set(ram_payer, key, *row);
   }
   queue_step(get_self(), CLOSEENV_QUEUE_LIMIT).run();
}

// ---------------------------------------------------------------------------
//  Configuration and the queue
// ---------------------------------------------------------------------------

void synd::setconfig(sysio::slug_name chain_code, sysio::slug_name token_code, uint32_t synd_fee_bps,
                     uint32_t desynd_fee_bps, uint64_t synd_burst, uint64_t synd_refill, uint64_t desynd_burst,
                     uint64_t desynd_refill, uint32_t window_sec, uint64_t bounty, uint64_t challenge_extra) {
   require_auth(SYSTEM_ACCOUNT);
   check(outpost_kind(chain_code).has_value(), not_outpost_message.data());
   const auto st = liq::find_stat_by_token(LIQ_ACCOUNT, token_code);
   check(st.has_value(), no_shadow_reason.data());
   check(st->chain_code == chain_code, other_chain_reason.data());
   check(synd_fee_bps <= bond::BPS_DENOMINATOR && desynd_fee_bps <= bond::BPS_DENOMINATOR, fee_bps_message.data());
   check(window_sec > 0, window_sec_message.data());
   for (const uint64_t amount : { synd_burst, synd_refill, desynd_burst, desynd_refill, bounty, challenge_extra }) {
      check(amount <= max_asset_amount, config_range_message.data());
   }

   syndconfig_t configs(get_self());
   configs.set(ram_payer, config_key{.chain_code = chain_code.value, .token_code = token_code.value},
               synd_config{
                  .chain_code      = chain_code,
                  .token_code      = token_code,
                  .synd_fee_bps    = synd_fee_bps,
                  .desynd_fee_bps  = desynd_fee_bps,
                  .synd_burst      = synd_burst,
                  .synd_refill     = synd_refill,
                  .desynd_burst    = desynd_burst,
                  .desynd_refill   = desynd_refill,
                  .window_sec      = window_sec,
                  .bounty          = bounty,
                  .challenge_extra = challenge_extra,
               });
}

void synd::crank(uint32_t limit) {
   // Permissionless and never throwing: a step only moves what the rules already allow.
   queue_step(get_self(), limit).run();
}

// dropenv - the escape hatch for an envelope the queue can never request.
//
// Every SYNDICATION item's remainder is burned out of this contract's liq row in one burn (the WIRE the
// items earned stays in the pool as slack), every item is erased, and the envelope
// is INVALID: the queue's next step counts it finished and moves `queue_epoch` past it. Nothing is
// pulled here, so the burn may touch this contract's liq row freely.
void synd::dropenv(sysio::slug_name chain_code, sysio::slug_name token_code, uint32_t epoch_index) {
   require_auth(SYSTEM_ACCOUNT);
   andon::check_clear(andon::ANDON_ACCOUNT);   // a burn: waits for the cord to clear
   envelopes_t        envelopes(get_self());
   const envelope_key key = envelope_key_of(chain_code, token_code, epoch_index);
   auto               row = envelopes.try_get(key);
   check(row.has_value(), no_envelope_message.data());
   check((row->state == envelope_state::OPEN || row->state == envelope_state::WAITING) && row->request_id == 0,
         dropenv_state_message.data());
   check(liq::find_stat_by_token(LIQ_ACCOUNT, token_code).has_value(), no_shadow_reason.data());

   items_t        items(get_self());
   const item_key first{.chain_code = chain_code.value, .token_code = token_code.value, .epoch_index = epoch_index,
                        .id = 0};
   uint64_t       unreleased = 0;
   for (auto it = items.lower_bound(first); it != items.end();) {
      const item_key& ikey = it.key();
      if (ikey.chain_code != first.chain_code || ikey.token_code != first.token_code ||
          ikey.epoch_index != first.epoch_index) {
         break;
      }
      if (it->kind == item_kind::SYNDICATION) unreleased = opp::safe::add_sat_u64(unreleased, it->remaining);
      it = items.erase(it);
   }
   if (unreleased > 0) {
      action(active_of(get_self()), LIQ_ACCOUNT, burn_action, std::make_tuple(token_code, unreleased)).send();
   }
   row->burned = opp::safe::add_sat_u64(row->burned, unreleased);
   row->state  = envelope_state::INVALID;
   envelopes.set(same_payer, key, *row);
   note(chain_code, token_code, epoch_index, dropped_reason);
}

void synd::challenge(name challenger, sysio::slug_name chain_code, sysio::slug_name token_code,
                     uint32_t epoch_index) {
   require_auth(challenger);
   check(challenger != get_self() && challenger != BOND_ACCOUNT, challenger_role_msg.data());
   check(!has_code(challenger), challenger_code_msg.data());

   envelopes_t        envelopes(get_self());
   const envelope_key key = envelope_key_of(chain_code, token_code, epoch_index);
   auto               row = envelopes.try_get(key);
   check(row.has_value(), no_envelope_message.data());
   check(row->state != envelope_state::HELD, challenged_message.data());
   // A DONE envelope is challengeable too: its request may still be only BONDED, its window open, and
   // the bond exists exactly so that what was released can be taken back.
   check(row->state == envelope_state::REQUESTED || row->state == envelope_state::RELEASABLE ||
            row->state == envelope_state::DONE,
         unchallengeable_msg.data());

   // sysio.bond holds only an OPEN or BONDED request, so a second hold fails its state check; mirrored
   // here, before anything is charged, in this contract's words.
   const auto req = find_request(get_self(), *row);
   check(req.has_value(), no_request_message.data());
   check(req->state != bond::request_state::HELD, challenged_message.data());
   check(req->state == bond::request_state::OPEN || req->state == bond::request_state::BONDED,
         request_final_message.data());

   const auto st = liq::find_stat_by_token(LIQ_ACCOUNT, token_code);
   check(st.has_value(), no_shadow_reason.data());
   const uint32_t hold_bps  = bond::bondconfig_t(BOND_ACCOUNT).get_or_default(bond::bond_config{}).hold_bps;
   const uint64_t hold_bond = bond::hold_bond_of(req->covered, hold_bps);
   syndconfig_t   configs(get_self());
   const auto     config = configs.try_get(config_key{.chain_code = chain_code.value, .token_code = token_code.value});
   const uint64_t extra  = config ? config->challenge_extra : 0;
   check(extra <= max_asset_amount - hold_bond, charge_range_message.data());
   const uint64_t charge = hold_bond + extra;
   check(charge > 0, zero_charge_message.data());

   // The extra stays in the fee pot; its position settles before the charge that backs it arrives.
   add_to_feepot(get_self(), token_code, st->supply.symbol.code(), extra);
   action(active_of(challenger), LIQ_ACCOUNT, transfer_action,
          std::make_tuple(challenger, get_self(), asset{static_cast<int64_t>(charge), st->supply.symbol},
                          std::string{challenge_memo}))
      .send();
   // sysio.bond pulls the hold bond back out of this contract's row, where the charge put it.
   action(active_of(get_self()), BOND_ACCOUNT, hold_action, std::make_tuple(req->id, challenger)).send();

   row->state = envelope_state::HELD;
   envelopes.set(same_payer, key, *row);

   // The queue may already have moved past a DONE envelope: rewind it, so the next step refreshes the
   // held envelope and processes the ruling.
   ledger_t         ledger(get_self());
   const ledger_key lkey{.chain_code = chain_code.value, .token_code = token_code.value};
   auto             lrow = ledger.try_get(lkey);
   if (lrow && lrow->queue_epoch > epoch_index) {
      lrow->queue_epoch = epoch_index;
      ledger.set(same_payer, lkey, *lrow);
   }
}

// sweepyield - pull what sysio.liq owes this contract's row into the token's pool, and pay out the fee
// pot's share.
//
// The fee pot is protocol revenue, so the WIRE its position earned is the protocol's: it is taken out of
// the pool, as far as the pool covers it, and sent to `sysio`. The pull is queued first (custody
// obligation 3), so the WIRE the transfer sends has arrived by the time it runs. Caller-signed, so the
// checked pull and a plain transfer are allowed here.
void synd::sweepyield(sysio::slug_name token_code) {
   andon::check_clear(andon::ANDON_ACCOUNT);   // pays the fee pot's WIRE out to sysio
   const auto st = liq::find_stat_by_token(LIQ_ACCOUNT, token_code);
   check(st.has_value(), no_shadow_reason.data());
   const symbol_code code = st->supply.symbol.code();
   yieldpools_t      pools(get_self());
   const token_key   key{token_code.value};
   pool_row          pool   = pools.try_get(key).value_or(pool_row{.token_code = token_code});
   const bool        pulled = custody::pull(pool.pool, LIQ_ACCOUNT, get_self(), code) > 0;

   feepots_t pots(get_self());
   auto      pot      = pots.try_get(key);
   uint64_t  fee_wire = 0;
   if (pot) {
      fee_wire = custody::settle_and_take(pot->position, pot->balance, pool.pool, LIQ_ACCOUNT, code);
      pots.set(same_payer, key, *pot);
   }
   if (pulled || fee_wire > 0) pools.set(ram_payer, key, pool);
   if (fee_wire > 0) {
      action(active_of(get_self()), TOKEN_ACCOUNT, transfer_action,
             std::make_tuple(get_self(), SYSTEM_ACCOUNT, asset{static_cast<int64_t>(fee_wire), liq::WIRE_SYM},
                             std::string{fee_yield_memo}))
         .send();
   }
}

// ---------------------------------------------------------------------------
//  The parked hold
// ---------------------------------------------------------------------------

void synd::sweep(name account, ChainKind chain_kind) {
   andon::check_clear(andon::ANDON_ACCOUNT);
   const auto pubkey = linked_pubkey(account, chain_kind);
   check(pubkey.has_value(), no_link_message.data());
   deliver_parked(account, chain_kind, *pubkey);
}

void synd::linkswept(name account, ChainKind chain_kind, std::vector<char> pubkey) {
   require_auth(AUTHEX_ACCOUNT);
   // Inline from the user's own createlink: anything short of a deliverable pubkey returns quietly
   // rather than abort the link.
   if (!is_account(account) || account == get_self() || !pubkey_fits(chain_kind, pubkey)) return;
   // Never abort the link: while the cord is pulled the parked balance stays for `sweep` after the clear.
   if (andon::pulled(andon::ANDON_ACCOUNT)) {
      sysio::print(std::string(frozen_linkswept_note));
      return;
   }
   deliver_parked(account, chain_kind, pubkey);
}

// ---------------------------------------------------------------------------
//  Desyndication
// ---------------------------------------------------------------------------

void synd::desyndicate(name holder, asset quantity) {
   require_auth(holder);
   andon::check_clear(andon::ANDON_ACCOUNT);
   check(quantity.is_valid() && quantity.amount > 0, quantity_message.data());
   liq::stats                statstable(LIQ_ACCOUNT);
   const liq::currency_stats st = statstable.get(liq::symbol_key{quantity.symbol.code().raw()},
                                                 no_symbol_message.data());
   check(quantity.symbol == st.supply.symbol, precision_message.data());

   const auto kind = outpost_kind(st.chain_code);
   check(kind.has_value(), not_outpost_message.data());
   const auto pubkey = linked_pubkey(holder, *kind);
   check(pubkey.has_value(), holder_unlinked_msg.data());

   // The pair's desyndication bucket, ticked to the depot's current epoch, must hold the whole quantity;
   // it drops by the whole quantity, the fee included.
   syndconfig_t   configs(get_self());
   const auto     config = configs.try_get(config_key{.chain_code = st.chain_code.value,
                                                      .token_code = st.token_code.value});
   check(config.has_value(), desynd_unset_message.data());
   const uint64_t quantity_units = static_cast<uint64_t>(quantity.amount);
   bucket_row     bucket = tick_bucket(get_self(), st.chain_code, st.token_code, bucket_direction::DESYNDICATION,
                                       config->desynd_burst, config->desynd_refill, epoch::current_epoch_index());
   check(quantity_units <= bucket.level, desynd_budget_message.data());
   bucket.level -= quantity_units;
   buckets_t(get_self()).set(ram_payer, bucket_key_of(st.chain_code, st.token_code, bucket_direction::DESYNDICATION),
                             bucket);

   // The fee stays in this contract as part of the fee pot; only the rest is burned and released on the
   // outpost. A fee that rounds to zero leaves the whole quantity to burn.
   const uint64_t fee    = fee_of(quantity_units, config->desynd_fee_bps);
   const uint64_t amount = quantity_units - fee;
   check(amount > 0, desynd_net_message.data());
   // The depot's outstanding shadow once the burn below has run, read from `st` before it: the burn is
   // inline, so nothing has moved supply yet. The net was part of the holder's balance, so it is within
   // the supply and the difference cannot underflow.
   const uint64_t total_syndicated = liq::outstanding_of(LIQ_ACCOUNT, st) - amount;

   // The fee pot's position settles before the transfer that backs the fee arrives; no distribution runs
   // between this action and its inline transfer, so both settle at one index. The quantity moves to this
   // contract under the holder's authority and the net is burned out of this contract's row. sysio.liq
   // settles both rows first, so the holder keeps the yield it earned.
   add_to_feepot(get_self(), st.token_code, st.supply.symbol.code(), fee);
   action(active_of(holder), LIQ_ACCOUNT, transfer_action,
          std::make_tuple(holder, get_self(), quantity, std::string{desyndicate_memo})).send();
   action(active_of(get_self()), LIQ_ACCOUNT, burn_action, std::make_tuple(st.token_code, amount)).send();

   syndcounters_t counters(get_self());
   synd_counters  c          = counters.get_or_default(synd_counters{});
   const uint64_t request_id = c.next_request_id++;
   counters.set(c, ram_payer);

   const ledger_row lrow = add_to_ledger(get_self(), st.chain_code, st.token_code, 0, 0, amount);

   desyndlog_t desyndlog(get_self());
   desyndlog.set(ram_payer, desynd_key{request_id}, desynd_row{
      .request_id       = request_id,
      .chain_code       = st.chain_code,
      .token_code       = st.token_code,
      .amount           = amount,
      .desyndicated_sum = lrow.desyndicated_sum,
      .total_syndicated = total_syndicated,
   });

   opp::attestations::DesyndicateLIQ msg;
   msg.chain_code = st.chain_code.value;
   msg.user       = opp::types::ChainAddress{*kind, *pubkey};
   msg.amount     = opp::types::TokenAmount{st.token_code.value, static_cast<int64_t>(amount)};
   msg.request_id = request_id;
   msg.total_syndicated = total_syndicated;
   // `no_size{}`: raw protobuf bytes, the form the outpost decodes the attestation `data` field as
   // (the same encoding sysio.opreg's emitters use).
   std::vector<char> encoded;
   auto              out = zpp::bits::out{encoded, zpp::bits::no_size{}};
   (void)out(msg);

   action(active_of(get_self()), MSGCH_ACCOUNT, queueout_action,
          std::make_tuple(st.chain_code.value, AttestationType::ATTESTATION_TYPE_DESYNDICATE_LIQ, encoded))
      .send();
}

// ---------------------------------------------------------------------------
//  Launch ingestion
// ---------------------------------------------------------------------------

void synd::importsynd(sysio::slug_name chain_code, sysio::slug_name token_code, std::vector<import_credit> credits) {
   opp::require_privileged_self();
   check(epoch::in_bootstrap_window(), window_message.data());
   syndstate_t state(get_self());
   check(!state.get_or_default(synd_state{}).import_complete, finalized_message.data());
   const auto st = liq::find_stat_by_token(LIQ_ACCOUNT, token_code);
   check(st.has_value(), no_shadow_reason.data());
   check(st->chain_code == chain_code, other_chain_reason.data());
   const auto kind = outpost_kind(chain_code);
   check(kind.has_value(), not_outpost_message.data());

   for (const auto& credit : credits) {
      check(pubkey_fits(*kind, credit.pubkey), pubkey_misfit_reason.data());
      if (credit.amount == 0) continue;
      check(credit.amount <= static_cast<uint64_t>(opp::safe::depot_amount_max), amount_range_reason.data());
      credit_by_pubkey(token_code, st->supply.symbol.code(), *kind, credit.pubkey, credit.amount);
   }
}

void synd::importdone() {
   opp::require_privileged_self();
   syndstate_t state(get_self());
   synd_state  st = state.get_or_default(synd_state{});
   check(!st.import_complete, finalized_message.data());
   st.import_complete = true;
   state.set(st, ram_payer);
}

// ---------------------------------------------------------------------------
//  Internals
// ---------------------------------------------------------------------------

bool synd::admit_sequence(sysio::slug_name chain_code, uint64_t sequence, uint64_t outpost_epoch) {
   syndcursors_t    cursors(get_self());
   const cursor_key key{chain_code.value};
   const auto       cursor = cursors.try_get(key);
   if (cursor && sequence <= cursor->last_sequence) return false;
   synd_cursor updated = cursor.value_or(synd_cursor{.chain_code = chain_code});
   updated.last_sequence = sequence;
   if (outpost_epoch != 0) updated.last_epoch = outpost_epoch;
   cursors.set(ram_payer, key, updated);
   return true;
}

void synd::check_custody(std::string_view path, const liq::currency_stats& st, sysio::slug_name chain_code,
                         uint32_t epoch_index, uint64_t sequence, item_kind kind, uint64_t reported,
                         uint64_t expected) {
   if (reported >= expected) {
      if (reported > expected)
         sysio::print("sysio.synd::", std::string(path), std::string(excess_verdict), reported,
                      std::string(outstanding_label), expected, "\n");
      return;
   }

   sysio::print("sysio.synd::", std::string(path), std::string(shortfall_verdict), reported,
                std::string(outstanding_label), expected, "\n");
   // The key is unique: only an admitted message reaches here, and admit_sequence consumed its sequence.
   mismatches_t(get_self()).set(ram_payer, mismatch_key{.chain_code = chain_code.value, .sequence = sequence},
                                mismatch_row{
                                   .chain_code  = chain_code,
                                   .token_code  = st.token_code,
                                   .epoch_index = epoch_index,
                                   .sequence    = sequence,
                                   .kind        = kind,
                                   .reported    = reported,
                                   .expected    = expected,
                                   .at          = time_point_sec(current_time_point()),
                                });

   // `pull` refuses an actor it does not admit, which would abort the envelope: send it only when
   // `may_pull` admits this contract. A pulled cord would take the pull as a no-op; it is not sent.
   if (!andon::may_pull(andon::ANDON_ACCOUNT, get_self())) {
      sysio::print("sysio.synd::", std::string(path), std::string(cord_not_pulled_note), "\n");
      return;
   }
   if (andon::pulled(andon::ANDON_ACCOUNT)) {
      sysio::print("sysio.synd::", std::string(path), std::string(cord_already_note), "\n");
      return;
   }
   const std::string reason = std::string(shortfall_reason) + chain_code.to_string() + std::string(reason_separator) +
                              st.token_code.to_string() + std::string(sequence_label) + std::to_string(sequence);
   sysio::print("sysio.synd::", std::string(path), std::string(cord_pulled_note), reason, "\n");
   action(active_of(get_self()), andon::ANDON_ACCOUNT, pull_action, std::make_tuple(get_self(), reason)).send();
}

void synd::deliver_parked(name account, ChainKind chain_kind, const std::vector<char>& pubkey) {
   liq::stats   statstable(LIQ_ACCOUNT);
   parkeds_t    parked(get_self());
   yieldpools_t pools(get_self());
   for (auto it = statstable.begin(); it != statstable.end(); ++it) {
      const parked_key key = parked_key_of(it->token_code, chain_kind, pubkey);
      const auto       row = parked.try_get(key);
      if (!row) continue;
      const symbol      sym = it->supply.symbol;
      const symbol_code code = sym.code();

      // Pull first -- nothing this delivery queues may touch this contract's liq row ahead of the
      // claim (custody obligation 3) -- then take the row's banked WIRE out of what the pool covers.
      const token_key pkey{it->token_code.value};
      pool_row        pool = pools.try_get(pkey).value_or(pool_row{.token_code = it->token_code});
      custody::pull(pool.pool, LIQ_ACCOUNT, get_self(), code);
      parked_row     settled = *row;
      const uint64_t banked  = custody::settle_and_take(settled.position, settled.balance, pool.pool, LIQ_ACCOUNT,
                                                       code);
      pools.set(ram_payer, pkey, pool);
      // Whatever the pool could not cover (flooring dust of this contract's liq row, custody
      // obligation 5) stays banked: the row is kept at balance 0 with it, and a later delivery pays it
      // once the pool covers it. With nothing left the row goes.
      const uint64_t balance = settled.balance;
      if (settled.position.owed_wire > 0) {
         settled.balance = 0;
         parked.set(ram_payer, key, settled);
      } else {
         parked.erase(key);
      }

      if (balance > 0) {
         action(active_of(get_self()), LIQ_ACCOUNT, transfer_action,
                std::make_tuple(get_self(), account, asset{static_cast<int64_t>(balance), sym},
                                std::string{parked_delivery_memo}))
            .send();
      }
      credit_owed(get_self(), account, code, banked);
   }
}

void synd::credit_by_pubkey(sysio::slug_name token_code, symbol_code sym, ChainKind chain_kind,
                            const std::vector<char>& pubkey, uint64_t amount) {
   const name account = linked_account(chain_kind, pubkey);
   if (account != name{}) {
      action(active_of(get_self()), LIQ_ACCOUNT, mint_action, std::make_tuple(account, token_code, amount)).send();
      return;
   }
   // Parked: the position settles at the live index before the balance grows, and the shadow it
   // attributes arrives in this contract's own row by the mint sent right after (custody
   // obligation 0).
   park_shadow(get_self(), token_code, sym, chain_kind, pubkey, amount, 0);
   action(active_of(get_self()), LIQ_ACCOUNT, mint_action, std::make_tuple(get_self(), token_code, amount)).send();
}

} // namespace sysio
