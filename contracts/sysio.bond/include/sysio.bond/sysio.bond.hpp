#pragma once
/**
 * @file sysio.bond.hpp
 * @brief sysio.bond: generic underwriting of provable statements on the depot.
 *
 * An issuer registers a statement (a schema name and its bytes) together with a covered amount of a
 * depot-native token and an optional bounty. Underwriters bond the statement in increments of 0.01
 * token until the bonded amount reaches the covered amount; a challenge window then runs. When it
 * passes with no hold the request is APPROVED. The issuer may instead hold the request by posting a
 * hold bond, and `sysio` rules it VALID or INVALID. Approval and the rulings only change state and
 * record what each party may claim; they send no transfer and notify no one. Every payout is pulled
 * one account at a time through `claim`, so no party can block another's.
 *
 * Depot-native tokens only: WIRE on `sysio.token`, or a shadow LIQ symbol on `sysio.liq`. Every bond
 * and escrow of a shadow token embeds a shadow-custody `position`, so the WIRE yield the contract's
 * single `sysio.liq` holder row earns is attributed to the sub-holders whose shadow it holds.
 *
 * Emergency stop: while the `sysio.andon` cord is pulled, `claim` is refused -- every payout waits for
 * the cord to clear -- and everything else runs: requests, bonds, holds, approval and rulings, and every
 * transfer into this contract.
 *
 * Privileged: an action signed by an account pulls that account's tokens by an inline `transfer`
 * under its `active` authority. The issuer pays the RAM of its `requests` and `escrows` rows and an
 * underwriter that of its `bonds` row -- unless that account is itself privileged: a system contract
 * (`sysio.synd` issuing for an envelope) has no RAM quota of its own, so its rows bill the `sysio` RAM
 * pool instead. Later writes by other signers keep the row's payer. The contract's own singletons and
 * yield pools bill the `sysio` RAM pool.
 */

#include <sysio/sysio.hpp>
#include <sysio/crypto.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/slug_name.hpp>
#include <sysio/symbol.hpp>
#include <sysio/time.hpp>
#include <sysio/asset.hpp>
#include <sysio.opp.common/depot_native_token.hpp>
#include <string_view>
#include <sysio.opp.common/shadow_custody_types.hpp>

#include <magic_enum/magic_enum.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace sysio {

namespace bond_validation {
inline constexpr std::string_view bond_increment_msg = "amount must be a positive multiple of the bond increment";
inline constexpr std::string_view precision_msg = "token precision is below the bond increment";
inline constexpr std::string_view unsupported_token_msg = "unsupported bond token";
inline constexpr std::string_view statement_len_msg = "statement exceeds the maximum length";
inline constexpr std::string_view duplicate_msg = "a request for this statement already exists";
inline constexpr std::string_view window_msg = "window must be positive";
inline constexpr std::string_view amount_range_msg = "amount exceeds the asset range";
} // namespace bond_validation


   class [[sysio::contract("sysio.bond")]] bond : public contract {
   public:
      using contract::contract;

      /// Ruling and configuration authority; council proposals execute as it.
      static constexpr name SYSTEM_ACCOUNT = "sysio"_n;
      /// Custody contract of WIRE.
      static constexpr name TOKEN_ACCOUNT  = "sysio.token"_n;
      /// Custody contract of the shadow LIQ symbols, and the source of their yield index.
      static constexpr name LIQ_ACCOUNT    = "sysio.liq"_n;

      /// Bonds move in steps of 10^-BOND_INCREMENT_DECIMALS token: the increment of a token with
      /// precision `p` is `10^(p - BOND_INCREMENT_DECIMALS)` base units, and a token with a precision
      /// below this is refused.
      static constexpr uint8_t  BOND_INCREMENT_DECIMALS = 2;
      /// Longest statement a request carries, in bytes.
      static constexpr uint32_t MAX_STATEMENT_BYTES     = 1024;
      /// Hold bond, in basis points of the covered amount, until `setconfig` sets another.
      static constexpr uint32_t DEFAULT_HOLD_BPS        = 1000;
      /// Minimum retention for ordinary requests. Durable requests additionally require issuer acknowledgement.
      static constexpr uint32_t PRUNE_RETENTION_SEC     = 7 * 24 * 60 * 60;
      /// One whole, in basis points.
      static constexpr uint32_t BPS_DENOMINATOR         = 10000;
      /// Base of the decimal increment arithmetic.
      static constexpr uint64_t DECIMAL_BASE            = 10;

      /// The bond increment of a token denominated in `sym`: `10^(precision - BOND_INCREMENT_DECIMALS)`
      /// base units, or `std::nullopt` when the precision is below BOND_INCREMENT_DECIMALS. A symbol's
      /// precision is at most 18, so the power fits in 64 bits. The ONE increment computation: this
      /// contract's `request` and `accept`, and every issuer that must know a covered amount is
      /// acceptable before it sends `request`, use it.
      static std::optional<uint64_t> increment_of(symbol sym) {
         if (sym.precision() < BOND_INCREMENT_DECIMALS) return std::nullopt;
         uint64_t increment = 1;
         for (uint8_t d = BOND_INCREMENT_DECIMALS; d < sym.precision(); ++d) increment *= DECIMAL_BASE;
         return increment;
      }

      /// sha256 over `schema` (its 8-byte value) then the statement bytes: a request's statement digest.
      /// The ONE digest computation, shared with issuers that check for a duplicate before `request`.
      static checksum256 statement_digest_of(name schema, const std::vector<char>& statement) {
         std::vector<char> buf(sizeof(uint64_t) + statement.size());
         const uint64_t    schema_value = schema.value;
         std::memcpy(buf.data(), &schema_value, sizeof(schema_value));
         if (!statement.empty()) std::memcpy(buf.data() + sizeof(schema_value), statement.data(), statement.size());
         return sha256(buf.data(), buf.size());
      }

      /// `amount * pot / covered` in 128 bits, floored: the share of `pot` a stake of `amount` out of
      /// `covered` earns. `amount <= covered` and `covered > 0` (a request refuses zero), so the share is
      /// at most `pot`. The ONE share computation: every payout of a bounty or a hold bond, and every
      /// issuer that must know what a ruling awards it, use it.
      static uint64_t share_of(uint64_t amount, uint64_t pot, uint64_t covered) {
         return static_cast<uint64_t>(static_cast<uint128_t>(amount) * pot / covered);
      }

      /// `covered * hold_bps / BPS_DENOMINATOR` in 128 bits, floored: the hold bond of a request covering
      /// `covered` base units at `hold_bps`. `hold_bps <= BPS_DENOMINATOR`, so it is at most `covered`.
      /// The ONE hold-bond computation, shared with an issuer that charges its own challenger for it.
      static uint64_t hold_bond_of(uint64_t covered, uint32_t hold_bps) {
         return static_cast<uint64_t>(static_cast<uint128_t>(covered) * hold_bps / BPS_DENOMINATOR);
      }

      /// Lifecycle of a request. APPROVED, VALID and INVALID are terminal.
      enum class request_state : uint8_t {
         OPEN,       ///< created; bonded below covered
         BONDED,     ///< bonded equals covered; the challenge window runs from `bonded_at`
         APPROVED,   ///< the window passed with no hold
         HELD,       ///< the issuer posted a hold bond; awaiting a ruling
         VALID,      ///< `sysio` ruled the statement true
         INVALID     ///< `sysio` ruled the statement false
      };

      /// What an escrow row holds for a request.
      enum class escrow_kind : uint8_t {
         BOUNTY,     ///< the issuer's bounty, paid to the underwriters or back per the ruling
         HOLD_BOND   ///< the issuer's hold bond, posted by `hold`
      };

      // -----------------------------------------------------------------------
      //  Configuration
      // -----------------------------------------------------------------------

      /// Set the hold bond, in basis points of the covered amount, for holds from now on. Above 0 and
      /// at most BPS_DENOMINATOR. Auth=sysio.
      [[sysio::action]] void setconfig(uint32_t hold_bps);

      // -----------------------------------------------------------------------
      //  Requests
      // -----------------------------------------------------------------------

      /// Create an OPEN request for `statement` under `schema`, covering `covered` base units of
      /// `token_code` (a positive multiple of the token's increment), with a challenge window of
      /// `window_sec` seconds once fully bonded. Pulls `bounty` of the token from `issuer` into
      /// escrow. One request per issuer and statement. Auth=issuer.
      [[sysio::action]] void request(name issuer, name schema, std::vector<char> statement,
                                     sysio::slug_name token_code, uint64_t covered, uint64_t bounty,
                                     uint32_t window_sec);

      /// Issue a request with durable terminal delivery. Identical to `request`, except pruning also
      /// requires the issuer's `ack`. Rulings and claims remain independent of acknowledgement.
      [[sysio::action]] void requestkeep(name issuer, name schema, std::vector<char> statement,
                                       sysio::slug_name token_code, uint64_t covered, uint64_t bounty,
                                       uint32_t window_sec);

      /// Acknowledge a terminal outcome after durably consuming it. Auth=issuer; idempotent.
      [[sysio::action]] void ack(uint64_t request_id);

      /// Raise the bounty of request `request_id`, not yet terminal, by `amount`, pulled from the
      /// issuer. Auth=issuer.
      [[sysio::action]] void addbounty(uint64_t request_id, uint64_t amount);

      /// Bond `amount` of request `request_id`'s token (a positive multiple of the increment) as
      /// `underwriter`, reduced to what remains uncovered. The request becomes BONDED when bonded
      /// reaches covered. Auth=underwriter.
      [[sysio::action]] void accept(name underwriter, uint64_t request_id, uint64_t amount);

      /// Hold request `request_id`, OPEN or BONDED, naming `beneficiary`, an existing account other
      /// than this contract, for a forfeit: pulls `covered * hold_bps / BPS_DENOMINATOR` from the
      /// issuer. The request becomes HELD. Auth=issuer.
      [[sysio::action]] void hold(uint64_t request_id, name beneficiary);

      /// Approve request `request_id`, BONDED with its window passed: a state change only, with no
      /// transfer and no notification. Permissionless.
      [[sysio::action]] void approve(uint64_t request_id);

      /// Rule request `request_id`, not terminal and bonded or not, VALID. Sends nothing: the bonds
      /// stay for their underwriters to claim with their stake's share of the bounty and the hold
      /// bond, and the escrows record the unbonded share of the bounty for `sysio` and the unbonded
      /// share of the hold bond for the issuer, each to claim. Auth=sysio.
      [[sysio::action]] void rslvvalid(uint64_t request_id);

      /// Rule request `request_id`, not terminal and bonded or not, INVALID. Sends nothing: the
      /// request records the whole bonded amount as the issuer's forfeit, and the escrows record the
      /// hold bond and the bounty for the hold beneficiary after a hold, otherwise the bounty for the
      /// issuer, each to claim. Auth=sysio.
      [[sysio::action]] void rslvinvalid(uint64_t request_id);

      // -----------------------------------------------------------------------
      //  Yield and payouts
      // -----------------------------------------------------------------------

      /// Pull the WIRE this contract's `sysio.liq` holder row has earned for the shadow token
      /// `token_code` into its yield pool. WIRE is refused. Permissionless.
      [[sysio::action]] void sweepyield(sysio::slug_name token_code);

      /// Pay `account` what terminal request `request_id` owes it, in one call:
      ///   - to the issuer of an INVALID request, the forfeit: the whole bonded amount in ONE
      ///     LIQ settlement (a `transfer` with memo `sysio.bond::forfeit` for WIRE);
      ///   - for its bond on an APPROVED or VALID request, the bond plus its stake's share of the
      ///     bounty and the hold bond, and the WIRE the bond earned; for its bond on an INVALID
      ///     request, nothing -- the WIRE the forfeited bond earned is paid into `sysio.liq` as bonus
      ///     yield of the token, or kept by the contract while the token has no supply;
      ///   - what a ruling awarded it out of an escrow (`escrow_row::payout`);
      ///   - for an escrow it funded, the WIRE the escrow earned.
      /// LIQ uses callback-free settlement; earned WIRE is recorded for separate `claimwire`.
      /// WIRE-denominated requests retain ordinary transfers. Residual unpaid yield prevents pruning.
      /// Refused when nothing is owed, and while the `sysio.andon` cord is pulled (what is owed stays
      /// recorded until the cord clears). Permissionless.
      [[sysio::action]] void claim(uint64_t request_id, name account);

      /// Withdraw the backed WIRE yield balance recorded by LIQ claims. Permissionless, fixed payee;
      /// a rejecting recipient leaves this balance intact without blocking LIQ settlement.
      [[sysio::action]] void claimwire(name account);

      struct wire_key {
         name account;
         SYSLIB_SERIALIZE(wire_key, (account))
      };
      struct [[sysio::table("wireclaims")]] wire_claim {
         name account;
         uint64_t amount = 0;
         SYSLIB_SERIALIZE(wire_claim, (account)(amount))
      };
      using wireclaims_t = kv::table<"wireclaims"_n, wire_key, wire_claim>;

      /// Walk the terminal requests from id `from_id` up and act on at most `limit` of them. A request
      /// whose rows are all paid or owe nothing, and that was ruled at least PRUNE_RETENTION_SEC ago, is
      /// erased with its bond and escrow rows. Any other terminal request keeps its row, but the bond rows
      /// already paid out are erased, so an unclaimed payout holds no other underwriter's RAM. A request
      /// whose token no longer resolves is skipped, never touched. Permissionless.
      [[sysio::action]] void prune(uint64_t from_id, uint32_t limit);

      // -----------------------------------------------------------------------
      //  Tables
      // -----------------------------------------------------------------------

      /// sha256 over `issuer` (its 8-byte value) then `statement_digest`: the key of the `bystatement`
      /// index. The ONE computation, shared with issuers that check for a duplicate before `request`.
      static checksum256 statement_key_of(name issuer, const checksum256& statement_digest) {
         std::array<char, sizeof(uint64_t) + sizeof(checksum256)> buf{};
         const uint64_t issuer_value = issuer.value;
         const auto     digest       = statement_digest.extract_as_byte_array();
         std::memcpy(buf.data(), &issuer_value, sizeof(issuer_value));
         std::memcpy(buf.data() + sizeof(issuer_value), digest.data(), digest.size());
         return sha256(buf.data(), buf.size());
      }

      /// Key of `requests`.
      struct request_key {
         uint64_t id;   ///< request id, from `bondcounters`
         SYSLIB_SERIALIZE(request_key, (id))
      };

      /// One underwriting request.
      struct [[sysio::table("requests")]] request_row {
         uint64_t          id;                 ///< request id
         name              issuer;             ///< account that registered the statement
         name              schema;             ///< how `statement` is to be read
         std::vector<char> statement;          ///< the statement's bytes, at most MAX_STATEMENT_BYTES
         checksum256       statement_digest;   ///< sha256 over schema then statement
         sysio::slug_name  token_code;         ///< registry code of the bond token
         uint64_t          covered;            ///< base units the underwriters must bond in total
         uint64_t          bonded;             ///< base units bonded so far; never above `covered`
         uint64_t          bounty;             ///< base units of bounty in escrow
         uint32_t          window_sec;         ///< challenge window after full bonding, in seconds
         request_state     state;              ///< lifecycle state
         time_point        created_at;         ///< when `request` ran
         time_point        bonded_at;          ///< when bonded reached covered; zero until then
         uint64_t          hold_bond;          ///< 0 until `hold`
         name              hold_beneficiary;   ///< who a forfeit pays after a hold; empty until `hold`
         time_point        held_at;            ///< when `hold` ran; zero until then
         time_point        resolved_at;        ///< when the request became terminal; zero until then
         /// `sysio.liq`'s yield index of the token when `rslvinvalid` ruled the request; 0 otherwise
         /// and for WIRE. A forfeited bond became the issuer's at that ruling, so its position is
         /// settled at this index, never later; what the forfeit earns while it waits for the issuer's
         /// claim is slack. Returned bonds and escrows are still their owners' and settle at the live
         /// index when claimed.
         uint128_t         resolved_index;
         /// Base units the issuer may claim as the forfeit of an INVALID request: the bonded amount
         /// from `rslvinvalid` until the issuer's `claim` pays it in one transfer, then 0. 0 in every
         /// other state.
         uint64_t          forfeit_pending;

         bool outcome_acknowledged = true; ///< false for durable requests until the issuer acknowledges

         /// The key of the `bystatement` index, which admits one request per issuer and statement.
         checksum256 by_statement() const { return statement_key_of(issuer, statement_digest); }

         SYSLIB_SERIALIZE(request_row, (id)(issuer)(schema)(statement)(statement_digest)(token_code)(covered)(bonded)
                          (bounty)(window_sec)(state)(created_at)(bonded_at)(hold_bond)(hold_beneficiary)(held_at)
                          (resolved_at)(resolved_index)(forfeit_pending)(outcome_acknowledged))
      };

      /// Secondary index of `requests` on `request_row::by_statement` -- the one declaration of its name.
      static constexpr name STATEMENT_INDEX = "bystatement"_n;

      /// Requests by id, with the issuer-and-statement index.
      using requests_t = kv::table<"requests"_n, request_key, request_row,
         kv::index<STATEMENT_INDEX, const_mem_fun<request_row, checksum256, &request_row::by_statement>>>;

      /// Resolve the stable issuer-and-statement identity without exposing index layout to clients.
      static std::optional<request_row> find_request(name code, name issuer, name schema,
                                                    const std::vector<char>& statement) {
         requests_t requests(code);
         const auto index = requests.get_index<STATEMENT_INDEX>();
         const auto it = index.find(statement_key_of(issuer, statement_digest_of(schema, statement)));
         return it == index.end() ? std::nullopt : std::optional<request_row>{*it};
      }

      /// Shared admission checks. Empty means admissible; the refusal is safe for intake to report
      /// without throwing. Transfer authority and available bounty remain the caller's responsibility.
      static std::string_view request_refusal(name code, name issuer, name schema,
                                             const std::vector<char>& statement, sysio::slug_name token_code,
                                             uint64_t covered, uint64_t bounty, uint32_t window_sec) {
         if (statement.size() > MAX_STATEMENT_BYTES) return bond_validation::statement_len_msg;
         const auto token = opp::custody::resolve_depot_native_token(LIQ_ACCOUNT, TOKEN_ACCOUNT, token_code);
         if (!token) return bond_validation::unsupported_token_msg;
         const auto increment = increment_of(token->sym);
         if (!increment) return bond_validation::precision_msg;
         if (covered == 0 || covered % *increment != 0)
            return bond_validation::bond_increment_msg;
         if (covered > static_cast<uint64_t>(asset::max_amount) || bounty > static_cast<uint64_t>(asset::max_amount))
            return bond_validation::amount_range_msg;
         if (window_sec == 0) return bond_validation::window_msg;
         if (find_request(code, issuer, schema, statement)) return bond_validation::duplicate_msg;
         return {};
      }

      /// Key of `bonds`.
      struct bond_key {
         uint64_t request_id;    ///< the request bonded
         name     underwriter;   ///< the bonding account
         SYSLIB_SERIALIZE(bond_key, (request_id)(underwriter))
      };

      /// One underwriter's bond on one request.
      struct [[sysio::table("bonds")]] bond_row {
         uint64_t                       request_id;    ///< the request bonded
         name                           underwriter;   ///< the bonding account
         uint64_t                       amount;        ///< base units bonded
         opp::shadow::custody::position yield;         ///< shadow yield of `amount`; untouched for WIRE
         /// Set once `claim` paid this bond out, or paid the WIRE a forfeited bond earned into
         /// `sysio.liq`.
         bool                           paid = false;
         SYSLIB_SERIALIZE(bond_row, (request_id)(underwriter)(amount)(yield)(paid))
      };

      /// Bonds by request and underwriter.
      using bonds_t = kv::table<"bonds"_n, bond_key, bond_row>;

      /// Key of `escrows`.
      struct escrow_key {
         uint64_t request_id;   ///< the request the escrow belongs to
         uint8_t  kind;         ///< magic_enum::enum_integer of the escrow_kind; the row keeps the enum
         SYSLIB_SERIALIZE(escrow_key, (request_id)(kind))
      };

      /// Tokens the issuer placed in escrow for one request.
      struct [[sysio::table("escrows")]] escrow_row {
         uint64_t                       request_id;    ///< the request the escrow belongs to
         escrow_kind                    kind;          ///< bounty or hold bond
         name                           funder;        ///< account the tokens came from
         /// Base units held: lowered as a claim pays a share or the `payout` out, and what is left
         /// after every share and the payout are paid is the division remainder, which stays in the
         /// contract as slack.
         uint64_t                       amount;
         opp::shadow::custody::position yield;         ///< shadow yield of `amount`; untouched for WIRE
         /// Set by the funder's `claim` that pays it the yield the escrow earned once no bond is owed a
         /// share of the escrow any more (every bond row of an APPROVED or VALID request is paid; an
         /// INVALID request pays no shares) and the payout is claimed: what is left, the division
         /// remainder, is slack, even when not zero. Until then the funder may claim again as what is
         /// still held keeps earning.
         bool                           paid = false;
         /// Account a ruling awarded `payout` of this escrow to: `sysio` or the issuer on VALID, the
         /// hold beneficiary or the issuer on INVALID. Empty until a ruling awards any.
         name                           payee;
         /// Base units of `amount` `payee` may claim; 0 until a ruling awards it and once claimed.
         uint64_t                       payout = 0;
         SYSLIB_SERIALIZE(escrow_row, (request_id)(kind)(funder)(amount)(yield)(paid)(payee)(payout))
      };

      /// Escrows by request and kind.
      using escrows_t = kv::table<"escrows"_n, escrow_key, escrow_row>;

      /// The key of request `request_id`'s escrow of `kind`.
      static escrow_key escrow_key_of(uint64_t request_id, escrow_kind kind) {
         return escrow_key{.request_id = request_id, .kind = magic_enum::enum_integer(kind)};
      }

      /// Principal still awarded to an account by settlement, excluding its underwriting stake and
      /// escrow yield. Clients need not interpret escrow rows or their paid flags.
      static uint64_t settlement_due(name code, uint64_t request_id, name account) {
         requests_t requests(code);
         const auto req = requests.try_get(request_key{request_id});
         if (!req) return 0;
         uint64_t due = req->issuer == account ? req->forfeit_pending : 0;
         escrows_t escrows(code);
         for (const auto kind : magic_enum::enum_values<escrow_kind>()) {
            const auto escrow = escrows.try_get(escrow_key_of(request_id, kind));
            if (escrow && escrow->payee == account) due += escrow->payout;
         }
         return due;
      }

      /// Key of `yieldpool`.
      struct pool_key {
         uint64_t token_code;   ///< the shadow token's registry code value
         SYSLIB_SERIALIZE(pool_key, (token_code))
      };

      /// The shadow-custody solvency pool of one shadow token.
      struct [[sysio::table("yieldpool")]] pool_row {
         sysio::slug_name                 token_code;   ///< registry code of the shadow token
         opp::shadow::custody::yield_pool pool;         ///< WIRE pulled from `sysio.liq` and credited out
         SYSLIB_SERIALIZE(pool_row, (token_code)(pool))
      };

      /// Yield pools by token code.
      using yieldpools_t = kv::table<"yieldpool"_n, pool_key, pool_row>;

      /// Governance-set configuration.
      struct [[sysio::table("bondconfig")]] bond_config {
         uint32_t hold_bps = DEFAULT_HOLD_BPS;   ///< hold bond, in basis points of the covered amount
         SYSLIB_SERIALIZE(bond_config, (hold_bps))
      };

      /// The configuration singleton.
      using bondconfig_t = kv::global<"bondconfig"_n, bond_config>;

      /// Id allocation.
      struct [[sysio::table("bondcounters")]] bond_counters {
         uint64_t next_request_id = 1;   ///< id the next `request` takes
         SYSLIB_SERIALIZE(bond_counters, (next_request_id))
      };

      /// The counters singleton.
      using bondcounters_t = kv::global<"bondcounters"_n, bond_counters>;
   };

} // namespace sysio
