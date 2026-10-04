#include <boost/test/unit_test.hpp>
#include <sysio/testing/tester.hpp>
#include <sysio/chain/abi_serializer.hpp>
#include <sysio/chain/kv_table_objects.hpp>
#include <sysio/opp/opp.hpp>
#include <sysio/opp/attestations/attestations.pb.h>
#include <sysio/opp/bootstrap/bootstrap.pb.h>
#include <sysio/opp/types/types.pb.h>
#include <google/protobuf/util/json_util.h>

#include <fc/crypto/base58.hpp>
#include <fc/crypto/elliptic_ed.hpp>
#include <fc/crypto/elliptic_em.hpp>
#include <fc/crypto/ethereum/ethereum_types.hpp>
#include <fc/crypto/hex.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/slug_name.hpp>
#include <fc/variant_object.hpp>

#include "contracts.hpp"
#include "contract_test_support.hpp"
#include "shadow_yield_reference.hpp"
#include "external_chain_simulator.hpp"
#include <random>

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace sysio::testing;
using namespace sysio;
using namespace sysio::chain;
using namespace sysio::opp::types;
using namespace fc;

using mvo = fc::mutable_variant_object;
using sysio_system::test_support::codename_mvo;

namespace {

/// `envelope_state` names as the ABI renders them.
constexpr std::string_view STATE_OPEN       = "OPEN";
constexpr std::string_view STATE_WAITING    = "WAITING";
constexpr std::string_view STATE_REQUESTED  = "REQUESTED";
constexpr std::string_view STATE_RELEASABLE = "RELEASABLE";
constexpr std::string_view STATE_HELD       = "HELD";
constexpr std::string_view STATE_INVALID    = "INVALID";
constexpr std::string_view STATE_DONE       = "DONE";
/// `sysio.bond` request states as the ABI renders them.
constexpr std::string_view REQUEST_OPEN     = "OPEN";
constexpr std::string_view REQUEST_BONDED   = "BONDED";
constexpr std::string_view REQUEST_APPROVED = "APPROVED";
constexpr std::string_view REQUEST_HELD     = "HELD";
/// The `total_syndicated` an intake helper carries by default: the whole asset range, at or above any
/// outstanding shadow (supply plus parked yield) a test can reach, so an outpost custody of this size
/// always covers the depot.
constexpr uint64_t CUSTODY_COVERING_ANY_SUPPLY = static_cast<uint64_t>(asset::max_amount);
/// `item_kind` names as the ABI renders them.
constexpr std::string_view KIND_SYNDICATION = "SYNDICATION";
constexpr std::string_view KIND_YIELD       = "YIELD";

/// Console fragments of sysio.synd's drop diagnostics.
constexpr std::string_view DROP_REPLAY       = "sysio.synd::onsynd: DROP -- replayed sequence";
constexpr std::string_view DROP_YIELD_REPLAY = "sysio.synd::onyield: DROP -- replayed sequence";
constexpr std::string_view DROP_NO_SHADOW    = "sysio.synd::onsynd: DROP -- token_code has no shadow symbol";
constexpr std::string_view DROP_OTHER_CHAIN  = "sysio.synd::onsynd: DROP -- token_code belongs to another chain";
constexpr std::string_view DROP_RANGE        = "sysio.synd::onsynd: DROP -- amount out of range";
constexpr std::string_view DROP_HEADROOM     = "sysio.synd::onsynd: DROP -- supply exceeds the asset range";
constexpr std::string_view DROP_PUBKEY       = "sysio.synd::onsynd: DROP -- pubkey does not fit the chain family";
constexpr std::string_view DROP_ENVELOPE_RANGE = "sysio.synd::onsynd: DROP -- the envelope's total, rounded up "
                                                 "to the bond increment, would exceed the asset range";
constexpr std::string_view DROP_YIELD_ENVELOPE_RANGE = "sysio.synd::onyield: DROP -- the envelope's total, rounded "
                                                       "up to the bond increment, would exceed the asset range";
constexpr std::string_view DROP_YIELD_HEADROOM =
   "sysio.synd::onyield: DROP -- the envelope's yield would exceed the shadow's headroom";
constexpr std::string_view DROP_KIND =
   "sysio.synd::onsynd: DROP -- chain_kind is not the outpost's chain family";
constexpr std::string_view DROP_CLOSED =
   "sysio.synd::onsynd: DROP -- envelope already closed or of another digest";
constexpr std::string_view DROP_DIGEST =
   "sysio.synd::closeenv: DROP -- envelope digest does not match; left OPEN";
/// Console fragments of the queue step's diagnostics.
constexpr std::string_view QUEUE_UNSET_BUCKET =
   "-- no syndconfig row: the syndication bucket is unset, nothing releases";
constexpr std::string_view QUEUE_EMPTY_BUCKET = "-- syndication bucket is empty until it refills";
constexpr std::string_view QUEUE_YIELD_HEADROOM = "-- yield exceeds the shadow's headroom; the item waits";
constexpr std::string_view QUEUE_INVALID_BURNED = "-- request ruled INVALID; the envelope is burned and INVALID";
constexpr std::string_view QUEUE_INVALID_BURNING =
   "-- request ruled INVALID; burning the envelope's items continues next step, the pair waits";
/// Console fragments of the queue step and `linkswept` while the andon cord is pulled.
constexpr std::string_view QUEUE_FROZEN =
   "sysio.synd::queue: the andon cord is pulled; releases, deliveries and burns wait for the clear";
constexpr std::string_view QUEUE_FROZEN_SHARE = "-- the challenger's hold share waits for the andon cord to clear";
constexpr std::string_view QUEUE_FROZEN_BURN =
   "-- request ruled INVALID while the andon cord is pulled; the burn waits for the clear, the pair waits";
constexpr std::string_view LINKSWEPT_FROZEN =
   "sysio.synd::linkswept: the andon cord is pulled; the parked balance waits for sweep after the clear";
/// Console fragments of the solvency check (every verdict line carries one of the two words).
constexpr std::string_view SHORTFALL       = ": SHORTFALL -- reported ";
constexpr std::string_view EXCESS          = ": EXCESS -- reported ";
constexpr std::string_view CORD_PULLED     = ": CORD PULLED -- custody shortfall ";
constexpr std::string_view CORD_ALREADY    = ": CORD ALREADY PULLED -- the shortfall is recorded";
constexpr std::string_view CORD_NOT_PULLED = ": CORD NOT PULLED -- sysio.synd is not a registered puller of "
                                             "sysio.andon, or sysio.andon is not deployed";
/// The intake paths as the verdicts name them.
constexpr std::string_view ONSYND_PATH  = "onsynd";
constexpr std::string_view ONYIELD_PATH = "onyield";
/// `mismatch` kinds as the ABI renders them.
constexpr std::string_view MISMATCH_SYNDICATION = "SYNDICATION";
constexpr std::string_view MISMATCH_YIELD       = "YIELD";

/// The whole verdict line of the solvency check on `path`: `word` (SHORTFALL or EXCESS) with the carried
/// custody and the outstanding it was compared with.
std::string verdict(std::string_view path, std::string_view word, uint64_t reported, uint64_t outstanding) {
   return "sysio.synd::" + std::string(path) + std::string(word) + std::to_string(reported) + " outstanding " +
          std::to_string(outstanding);
}
/// The line a pull prints: the reason it carries.
std::string pulled_line(std::string_view path, std::string_view chain_code, std::string_view token_code,
                        uint64_t sequence) {
   return "sysio.synd::" + std::string(path) + std::string(CORD_PULLED) + std::string(chain_code) + " " +
          std::string(token_code) + " seq " + std::to_string(sequence);
}
/// The reason a shortfall's pull records on the cord.
std::string pull_reason(std::string_view chain_code, std::string_view token_code, uint64_t sequence) {
   return "custody shortfall " + std::string(chain_code) + " " + std::string(token_code) + " seq " +
          std::to_string(sequence);
}

/// Refusals of `challenge`.
constexpr std::string_view CHALLENGE_TWICE     = "the envelope's request is already challenged";
constexpr std::string_view CHALLENGE_STATE     = "only a REQUESTED, RELEASABLE or DONE envelope can be challenged";
constexpr std::string_view CHALLENGE_ZERO      = "challenge charge is zero; configure challenge_extra";
/// Refusal of `dropenv`.
constexpr std::string_view DROPENV_STATE = "only an OPEN or WAITING envelope with no request issued can be dropped";
constexpr std::string_view CHALLENGE_FINAL     = "the envelope's request is approved or ruled; it cannot be challenged";
constexpr std::string_view CHALLENGE_NO_ROW    = "envelope not found";
constexpr std::string_view CHALLENGE_ROLE      = "the challenger cannot be sysio.synd or sysio.bond";
/// sysio.bond's hold bond, in basis points of the covered amount, while no `setconfig` sets another.
constexpr uint64_t HOLD_BPS        = 1000;
constexpr uint64_t BPS_DENOMINATOR = 10000;

/// The schema every envelope statement is registered under on sysio.bond.
constexpr std::string_view STATEMENT_SCHEMA = "oppenvelope";
/// sysio.synd's default challenge window, for a pair with no `syndconfig` row.
constexpr uint32_t DEFAULT_WINDOW_SEC = 10800;
/// A crank budget large enough for every case here.
constexpr uint32_t CRANK_LIMIT = 100;

/// sysio.bond's PRUNE_RETENTION_SEC: a ruled request stays seven days.
constexpr uint32_t PRUNE_RETENTION_SEC = 604'800;

/// The largest amount the depot accepts: `opp::safe::depot_amount_max`.
constexpr uint64_t DEPOT_AMOUNT_MAX = (uint64_t{1} << 62) - 1;

/// The chain-native address `sysio.authex::recordlink` carries beside a linked key. On SVM that is the
/// ED key's own 32 bytes; on EVM it is the derived 20-byte address.
std::vector<char> native_address_of(const fc::crypto::public_key& pub_key) {
   if (pub_key.type() == fc::crypto::public_key::key_type::em) {
      const auto address = fc::crypto::ethereum::address_to_bytes(pub_key);
      return {address.begin(), address.end()};
   }
   const auto raw = pub_key.get<fc::crypto::ed::public_key_shim>().serialize();
   return std::vector<char>(raw.begin(), raw.end());
}

/// The dev bootstrap config, parsed as strictly as the bootstrap tool parses it.
sysio::opp::bootstrap::BootstrapPlatformConfig load_dev_config() {
   const auto    path = contracts::dex_config_dir() + "/dex-config.dev.json";
   std::ifstream in(path);
   BOOST_REQUIRE_MESSAGE(in.good(), "cannot open " << path);
   std::stringstream json;
   json << in.rdbuf();
   sysio::opp::bootstrap::BootstrapPlatformConfig cfg;
   google::protobuf::util::JsonParseOptions      opts;   // an unknown field is an error
   const auto status = google::protobuf::util::JsonStringToMessage(json.str(), &cfg, opts);
   BOOST_REQUIRE_MESSAGE(status.ok(), status.ToString());
   return cfg;
}

} // anonymous namespace

/// sysio.synd on the depot: the registries it validates against, sysio.liq as the ledger it mints into
/// and holds in, sysio.authex for the links a parked hold is delivered on (with sysio.dclaim, which
/// `createlink` requires), sysio.msgch as the signer of the envelope path and the outbound queue a
/// desyndication lands in (the routing through its code is exercised by `sysio_dispatch_tests`), and
/// sysio.swap for the launch replay's yield pools, sysio.bond as the underwriter of every envelope, and a
/// stand-in on sysio.epoch that moves the depot epoch index the buckets tick to.
class sysio_synd_tester : public tester {
public:
   static constexpr auto SYND_ACCOUNT   = "sysio.synd"_n;
   static constexpr auto LIQ_ACCOUNT    = "sysio.liq"_n;
   static constexpr auto MSGCH_ACCOUNT  = "sysio.msgch"_n;
   static constexpr auto AUTHEX_ACCOUNT = "sysio.authex"_n;
   static constexpr auto TOKEN_ACCOUNT  = "sysio.token"_n;
   static constexpr auto TOKENS_ACCOUNT = "sysio.tokens"_n;
   static constexpr auto CHAINS_ACCOUNT = "sysio.chains"_n;
   static constexpr auto DCLAIM_ACCOUNT = "sysio.dclaim"_n;
   static constexpr auto SWAP_ACCOUNT   = "sysio.swap"_n;
   static constexpr auto BOND_ACCOUNT   = "sysio.bond"_n;
   static constexpr auto EPOCH_ACCOUNT  = "sysio.epoch"_n;
   static constexpr auto SYSIO_ACCOUNT  = "sysio"_n;

   static inline const symbol WIRE_SYM   = symbol::from_string("9,WIRE");
   static inline const symbol LIQSOL_SYM = symbol::from_string("9,LIQSOL");
   static inline const symbol LIQTWO_SYM = symbol::from_string("9,LIQTWO");
   static inline const symbol LIQETH_SYM = symbol::from_string("9,LIQETH");
   static constexpr uint64_t  UNIT       = 1'000'000'000;   // one whole token of a precision-9 symbol
   /// `synd::bucket_direction` values, one byte each in a `buckets` key.
   static constexpr char      SYNDICATION_DIRECTION   = 0;
   static constexpr char      DESYNDICATION_DIRECTION = 1;

   static constexpr std::string_view SOLANA  = "SOLANA";
   static constexpr std::string_view ETH     = "ETH";
   static constexpr std::string_view LIQSOL  = "LIQSOL";
   static constexpr std::string_view LIQTWO  = "LIQTWO";
   static constexpr std::string_view LIQETH  = "LIQETH";
   static constexpr std::string_view LIQNONE = "LIQNONE";   ///< a liq token nobody opened a shadow for

   /// How the fixture sets up the emergency stop on sysio.andon.
   enum class andon_setup {
      puller,         ///< deployed, with sysio.synd registered as a puller, as the depot deploys it
      unregistered,   ///< deployed, with no puller registered
      absent          ///< no sysio.andon account at all: the cord reads clear and nobody may pull it
   };

   /// `seed_registries` registers the two outposts, their liq tokens and the shadows every case
   /// starts from; the launch-replay case replays a bootstrap config into empty registries instead.
   /// `cord` sets up sysio.andon.
   explicit sysio_synd_tester(bool seed_registries = true, andon_setup cord = andon_setup::puller) {
      produce_blocks(2);
      // sysio.authex is pre-created by the tester boot.
      create_accounts({ SYND_ACCOUNT, LIQ_ACCOUNT, MSGCH_ACCOUNT, TOKEN_ACCOUNT, TOKENS_ACCOUNT, CHAINS_ACCOUNT,
                        DCLAIM_ACCOUNT, SWAP_ACCOUNT, BOND_ACCOUNT, EPOCH_ACCOUNT, "alice"_n, "bob"_n, "carol"_n,
                        "dave"_n });
      produce_blocks(2);

      deploy(AUTHEX_ACCOUNT, contracts::authex_wasm(), contracts::authex_abi(), authex_abi_ser);
      deploy(DCLAIM_ACCOUNT, contracts::dclaim_wasm(), contracts::dclaim_abi(), dclaim_abi_ser);
      deploy(CHAINS_ACCOUNT, contracts::chains_wasm(), contracts::chains_abi(), chains_abi_ser);
      deploy(TOKENS_ACCOUNT, contracts::tokens_wasm(), contracts::tokens_abi(), tokens_abi_ser);
      deploy(TOKEN_ACCOUNT,  contracts::token_wasm(),  contracts::token_abi(),  token_abi_ser);
      deploy(MSGCH_ACCOUNT,  contracts::msgch_wasm(),  contracts::msgch_abi(),  msgch_abi_ser);
      // The swap is unprivileged, as it is deployed; it bills a user's rows to the user.
      deploy(SWAP_ACCOUNT,   contracts::swap_wasm(),   contracts::swap_abi(),   swap_abi_ser, false);
      deploy(LIQ_ACCOUNT,    contracts::liq_wasm(),    contracts::liq_abi(),    liq_abi_ser);
      deploy(SYND_ACCOUNT,   contracts::synd_wasm(),   contracts::synd_abi(),   synd_abi_ser);
      deploy(BOND_ACCOUNT,   contracts::bond_wasm(),   contracts::bond_abi(),   bond_abi_ser);
      deploy(EPOCH_ACCOUNT,  contracts::util::epoch_stub_wasm(), contracts::util::epoch_stub_abi(), epoch_abi_ser,
             false);
      if (cord != andon_setup::absent) {
         sysio_system::test_support::andon::deploy(*this, andon_abi_ser, contracts::andon_wasm(),
                                                   contracts::andon_abi());
         if (cord == andon_setup::puller)
            BOOST_REQUIRE_EQUAL(success(), push(sysio_system::test_support::andon::account, andon_abi_ser,
                                                SYSIO_ACCOUNT, "addpuller"_n, mvo()("contract", SYND_ACCOUNT)));
      }

      // WIRE: the treasury supply on sysio and a funder for yield distributions.
      BOOST_REQUIRE_EQUAL(success(), push(TOKEN_ACCOUNT, token_abi_ser, TOKEN_ACCOUNT, "create"_n, mvo()
         ("issuer", SYSIO_ACCOUNT)("maximum_supply", asset(1'000'000'000 * UNIT, WIRE_SYM))));
      BOOST_REQUIRE_EQUAL(success(), push(TOKEN_ACCOUNT, token_abi_ser, SYSIO_ACCOUNT, "issue"_n, mvo()
         ("to", SYSIO_ACCOUNT)("quantity", asset(1'000'000'000 * UNIT, WIRE_SYM))("memo", "")));
      BOOST_REQUIRE_EQUAL(success(), push(TOKEN_ACCOUNT, token_abi_ser, SYSIO_ACCOUNT, "transfer"_n, mvo()
         ("from", SYSIO_ACCOUNT)("to", "carol"_n)("quantity", asset(1'000'000 * UNIT, WIRE_SYM))("memo", "")));

      // The swap: governance is sysio, the system token WIRE.
      BOOST_REQUIRE_EQUAL(success(), push(SWAP_ACCOUNT, swap_abi_ser, SWAP_ACCOUNT, "setconfig"_n, mvo()
         ("fee_authority", SYSIO_ACCOUNT)("system_token", extended_symbol{WIRE_SYM, TOKEN_ACCOUNT})));

      if (!seed_registries) return;

      // Two outposts; SOLANA carries two shadowed liq tokens and one without a shadow.
      BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_SVM, SOLANA, 2));
      BOOST_REQUIRE_EQUAL(success(), regchain(ChainKind::CHAIN_KIND_EVM, ETH, 1));
      BOOST_REQUIRE_EQUAL(success(), regtoken(LIQSOL, ChainKind::CHAIN_KIND_SVM, SOLANA));
      BOOST_REQUIRE_EQUAL(success(), regtoken(LIQTWO, ChainKind::CHAIN_KIND_SVM, SOLANA));
      BOOST_REQUIRE_EQUAL(success(), regtoken(LIQNONE, ChainKind::CHAIN_KIND_SVM, SOLANA));
      BOOST_REQUIRE_EQUAL(success(), regtoken(LIQETH, ChainKind::CHAIN_KIND_EVM, ETH));
      BOOST_REQUIRE_EQUAL(success(), create_shadow(LIQSOL_SYM, SOLANA, LIQSOL));
      BOOST_REQUIRE_EQUAL(success(), create_shadow(LIQTWO_SYM, SOLANA, LIQTWO));
      BOOST_REQUIRE_EQUAL(success(), create_shadow(LIQETH_SYM, ETH, LIQETH));
   }

   // --- deployment and pushing ---

   /// Deploy `account`, privileged as every depot system contract is unless `privileged` is false, and
   /// load its ABI.
   void deploy(name account, const std::vector<uint8_t>& wasm, const std::vector<char>& abi, abi_serializer& ser,
               bool privileged = true) {
      set_code(account, wasm);
      set_abi(account, abi.data());
      if (privileged) set_privileged(account);
      produce_blocks();
      sysio_system::test_support::load_account_abi(*this, account, ser);
   }

   /// Push one action in its own block, keeping its trace in `last_trace` (null after a failure).
   action_result push(name code, abi_serializer& ser, name signer, name action_name, const variant_object& data) {
      return sysio_system::test_support::push_contract_action_and_produce_block(*this, code, ser, signer,
                                                                                action_name, data, last_trace);
   }
   static bool mentions(const action_result& r, std::string_view text) { return r.find(text) != std::string::npos; }
   /// Every action's console of the last pushed transaction, inline actions included.
   std::string console() const {
      std::string out;
      if (!last_trace) return out;
      for (const auto& at : last_trace->action_traces) out += at.console;
      return out;
   }
   bool console_has(std::string_view text) const { return console().find(text) != std::string::npos; }

   // --- slugs and keys ---

   static uint64_t slug_value(std::string_view s) { return fc::slug_name{s}.value; }

   static fc::crypto::public_key ed_key() {
      return fc::crypto::private_key::generate(fc::crypto::private_key::key_type::ed).get_public_key();
   }
   static std::vector<char> ed_bytes(const fc::crypto::public_key& pk) {
      const auto raw = pk.get<fc::crypto::ed::public_key_shim>().serialize();
      return std::vector<char>(raw.begin(), raw.end());
   }
   static fc::crypto::public_key em_key() {
      return fc::crypto::private_key::generate(fc::crypto::private_key::key_type::em).get_public_key();
   }
   /// The 33-byte compressed secp256k1 form an EVM outpost reports a user's key in.
   static std::vector<char> em_bytes(const fc::crypto::public_key& pk) {
      const auto raw = pk.get<fc::em::public_key_shim>().serialize();
      return std::vector<char>(raw.begin(), raw.end());
   }
   /// A canonical envelope digest standing in for the one `sysio.msgch` accepted.
   static fc::sha256 digest_of(std::string_view seed) { return fc::sha256::hash(std::string(seed)); }

   // --- registries ---

   // Registrations inside the epoch-0 bootstrap window land ACTIVE, as the launch bootstrap's do.
   action_result regchain(ChainKind kind, std::string_view code, uint32_t external_chain_id) {
      return push(CHAINS_ACCOUNT, chains_abi_ser, CHAINS_ACCOUNT, "regchain"_n, mvo()
         ("kind", kind)("code", codename_mvo(code))("external_chain_id", external_chain_id)
         ("name", std::string("outpost"))("description", std::string{})
         ("outpost", sysio_system::test_support::no_outpost_mvo()));
   }
   /// Register a liq token of `chain_code` with `precision` at `address` and bind it to the chain, as the
   /// bootstrap does from a `TokenSpec`.
   action_result regtoken(std::string_view code, uint32_t precision, ChainKind chain_kind,
                          std::string_view chain_code, const std::vector<char>& address) {
      auto r = push(TOKENS_ACCOUNT, tokens_abi_ser, TOKENS_ACCOUNT, "regtoken"_n, mvo()
         ("kind", TokenKind::TOKEN_KIND_LIQ)("code", codename_mvo(code))("symbol_name", std::string(code))
         ("description", std::string{})("precision", precision)
         ("address", mvo()("kind", chain_kind)("address", address)));
      if (r != success()) return r;
      return push(TOKENS_ACCOUNT, tokens_abi_ser, TOKENS_ACCOUNT, "regctok"_n, mvo()
         ("chain_code", codename_mvo(chain_code))("token_code", codename_mvo(code))("contract_addr", address)
         ("is_native", false));
   }
   /// Register a 9-decimal liq token of `chain_code` at a placeholder address and bind it to the chain.
   action_result regtoken(std::string_view code, ChainKind chain_kind, std::string_view chain_code) {
      return regtoken(code, 9, chain_kind, chain_code,
                      std::vector<char>(chain_kind == ChainKind::CHAIN_KIND_SVM ? 32 : 20, char(0x5a)));
   }
   /// The bytes behind a bootstrap config's display-form address or pubkey: `0x`-hex on EVM, base58 on
   /// SVM.
   static std::vector<char> address_bytes(ChainKind kind, const std::string& display) {
      if (kind == ChainKind::CHAIN_KIND_SVM) return fc::from_base58(display);
      BOOST_REQUIRE_MESSAGE(display.starts_with("0x"), "not a 0x-hex address: " << display);
      std::vector<char> out((display.size() - 2) / 2);
      out.resize(fc::from_hex(std::string_view(display).substr(2), out.data(), out.size()));
      return out;
   }
   action_result create_shadow(symbol sym, std::string_view chain_code, std::string_view token_code) {
      return push(LIQ_ACCOUNT, liq_abi_ser, LIQ_ACCOUNT, "create"_n, mvo()
         ("sym", sym)("chain_code", codename_mvo(chain_code))("token_code", codename_mvo(token_code)));
   }
   /// Link `account` to `key` on SVM through sysio.authex's trusted path.
   action_result link_svm(name account, const fc::crypto::public_key& key) {
      return push(AUTHEX_ACCOUNT, authex_abi_ser, AUTHEX_ACCOUNT, "recordlink"_n, mvo()
         ("account", account)("chain_kind", ChainKind::CHAIN_KIND_SVM)("pub_key", key)
         ("native_address", native_address_of(key)));
   }
   /// Link `account` to the key behind `priv` through a user-signed `sysio.authex::createlink`,
   /// which sends `linkswept` to sysio.synd inline.
   action_result createlink(name account, const fc::crypto::private_key& priv,
                            ChainKind kind = ChainKind::CHAIN_KIND_EVM) {
      const uint64_t nonce = control->head().block_time().time_since_epoch().count() / 1000;
      return push(AUTHEX_ACCOUNT, authex_abi_ser, account, "createlink"_n, mvo()
         ("chain_kind", kind)("account", account.to_string())
         ("sig", sysio_system::test_support::sign_createlink(priv, account.to_string(), kind,
                                                             nonce))
         ("pub_key", priv.get_public_key())("nonce", nonce));
   }

   // --- sysio.synd actions ---

   /// `total_syndicated` is the outpost custody the message carries.
   action_result onsynd(std::string_view chain_code, uint32_t epoch, const fc::sha256& digest, uint64_t sequence,
                        ChainKind kind, const std::vector<char>& pubkey, std::string_view token_code,
                        uint64_t amount, name signer = MSGCH_ACCOUNT,
                        uint64_t total_syndicated = CUSTODY_COVERING_ANY_SUPPLY) {
      return push(SYND_ACCOUNT, synd_abi_ser, signer, "onsynd"_n, mvo()
         ("chain_code", codename_mvo(chain_code))("epoch_index", epoch)("digest", digest)("sequence", sequence)
         ("chain_kind", kind)("pubkey", pubkey)("token_code", codename_mvo(token_code))("amount", amount)
         ("total_syndicated", total_syndicated));
   }
   /// `total_syndicated` is the outpost custody the report carries.
   action_result onyield(std::string_view chain_code, uint32_t epoch, const fc::sha256& digest, uint64_t sequence,
                         uint64_t outpost_epoch, std::string_view token_code, uint64_t amount,
                         name signer = MSGCH_ACCOUNT, uint64_t total_syndicated = CUSTODY_COVERING_ANY_SUPPLY) {
      return push(SYND_ACCOUNT, synd_abi_ser, signer, "onyield"_n, mvo()
         ("chain_code", codename_mvo(chain_code))("epoch_index", epoch)("digest", digest)("sequence", sequence)
         ("outpost_epoch", outpost_epoch)("token_code", codename_mvo(token_code))("amount", amount)
         ("total_syndicated", total_syndicated));
   }
   action_result closeenv(std::string_view chain_code, uint32_t epoch, const fc::sha256& digest,
                          name signer = MSGCH_ACCOUNT) {
      return push(SYND_ACCOUNT, synd_abi_ser, signer, "closeenv"_n, mvo()
         ("chain_code", codename_mvo(chain_code))("epoch_index", epoch)("digest", digest));
   }

   // --- the parked hold, desyndication and launch ingestion ---

   action_result sweep(name account, ChainKind kind, name signer = "alice"_n) {
      return push(SYND_ACCOUNT, synd_abi_ser, signer, "sweep"_n, mvo()("account", account)("chain_kind", kind));
   }
   action_result linkswept(name account, ChainKind kind, const std::vector<char>& pubkey,
                           name signer = AUTHEX_ACCOUNT) {
      return push(SYND_ACCOUNT, synd_abi_ser, signer, "linkswept"_n, mvo()
         ("account", account)("chain_kind", kind)("pubkey", pubkey));
   }
   action_result desyndicate(name holder, uint64_t amount, symbol sym = LIQSOL_SYM, name signer = name{}) {
      return push(SYND_ACCOUNT, synd_abi_ser, signer == name{} ? holder : signer, "desyndicate"_n, mvo()
         ("holder", holder)("quantity", asset(static_cast<int64_t>(amount), sym)));
   }
   static mvo credit(const std::vector<char>& pubkey, uint64_t amount) {
      return mvo()("pubkey", pubkey)("amount", amount);
   }
   action_result importsynd(std::string_view chain_code, std::string_view token_code, const fc::variants& credits,
                            name signer = SYND_ACCOUNT) {
      return push(SYND_ACCOUNT, synd_abi_ser, signer, "importsynd"_n, mvo()
         ("chain_code", codename_mvo(chain_code))("token_code", codename_mvo(token_code))("credits", credits));
   }
   action_result importdone(name signer = SYND_ACCOUNT) {
      return push(SYND_ACCOUNT, synd_abi_ser, signer, "importdone"_n, mvo());
   }

   // --- the queue ---

   /// The rules of one pair; the defaults are the spec's, with a bucket wide open.
   struct pair_config {
      uint32_t synd_fee_bps    = 0;
      uint32_t desynd_fee_bps  = 0;
      uint64_t synd_burst      = 1'000'000 * UNIT;
      uint64_t synd_refill     = 1'000'000 * UNIT;
      uint64_t desynd_burst    = 1'000'000 * UNIT;
      uint64_t desynd_refill   = 1'000'000 * UNIT;
      uint32_t window_sec      = DEFAULT_WINDOW_SEC;
      uint64_t bounty          = 0;
      uint64_t challenge_extra = 0;
   };
   action_result setconfig(std::string_view chain_code, std::string_view token_code, const pair_config& cfg,
                           name signer = SYSIO_ACCOUNT) {
      return push(SYND_ACCOUNT, synd_abi_ser, signer, "setconfig"_n, mvo()
         ("chain_code", codename_mvo(chain_code))("token_code", codename_mvo(token_code))
         ("synd_fee_bps", cfg.synd_fee_bps)("desynd_fee_bps", cfg.desynd_fee_bps)("synd_burst", cfg.synd_burst)
         ("synd_refill", cfg.synd_refill)("desynd_burst", cfg.desynd_burst)("desynd_refill", cfg.desynd_refill)
         ("window_sec", cfg.window_sec)("bounty", cfg.bounty)("challenge_extra", cfg.challenge_extra));
   }
   action_result dropenv(std::string_view chain_code, std::string_view token_code, uint32_t epoch,
                         name signer = SYSIO_ACCOUNT) {
      return push(SYND_ACCOUNT, synd_abi_ser, signer, "dropenv"_n, mvo()
         ("chain_code", codename_mvo(chain_code))("token_code", codename_mvo(token_code))("epoch_index", epoch));
   }
   action_result crank(uint32_t limit = CRANK_LIMIT, name signer = "carol"_n) {
      return push(SYND_ACCOUNT, synd_abi_ser, signer, "crank"_n, mvo()("limit", limit));
   }
   /// Move the depot's current epoch index, through the stand-in on sysio.epoch.
   action_result set_epoch(uint32_t index) {
      return push(EPOCH_ACCOUNT, epoch_abi_ser, EPOCH_ACCOUNT, "setindex"_n, mvo()("index", index));
   }

   // --- sysio.andon ---

   /// The sysio.andon `cord` row; null when sysio.andon is absent or has never been pulled.
   fc::variant cord_row() {
      if (control->find_account(sysio_system::test_support::andon::account) == nullptr) return fc::variant();
      return decode(andon_abi_ser, "cord_state",
                    get_row_by_account(sysio_system::test_support::andon::account,
                                       sysio_system::test_support::andon::account, "cord"_n, "cord"_n));
   }
   /// Whether the andon cord is pulled now.
   bool cord_pulled() {
      const auto row = cord_row();
      return !row.is_null() && row["pulled"].as_bool();
   }
   /// Every `mismatch` row, in key order: by outpost, then sequence.
   std::vector<fc::variant> mismatch_rows() {
      std::vector<fc::variant> rows;
      for (const auto& value : kv_rows("mismatch"_n)) rows.push_back(decode(synd_abi_ser, "mismatch_row", value));
      return rows;
   }
   /// The solvency check left no trace: no `mismatch` row and a clear cord.
   void require_no_shortfall() {
      BOOST_REQUIRE(mismatch_rows().empty());
      BOOST_REQUIRE(!cord_pulled());
      BOOST_REQUIRE(!console_has(SHORTFALL));
   }
   /// A `mismatch` row reports what it was compared with: `(chain_code, sequence)` of an item of `kind` of
   /// `token_code` in the envelope of `epoch`, `reported` custody below the `expected` outstanding, recorded at
   /// the block of the last pushed transaction.
   void require_mismatch(const fc::variant& row, std::string_view chain_code, std::string_view token_code,
                         uint32_t epoch, uint64_t sequence, std::string_view kind, uint64_t reported,
                         uint64_t expected) {
      BOOST_REQUIRE_EQUAL(chain_code, row["chain_code"].as_string());
      BOOST_REQUIRE_EQUAL(token_code, row["token_code"].as_string());
      BOOST_REQUIRE_EQUAL(epoch, row["epoch_index"].as<uint32_t>());
      BOOST_REQUIRE_EQUAL(sequence, row["sequence"].as_uint64());
      BOOST_REQUIRE_EQUAL(kind, row["kind"].as_string());
      BOOST_REQUIRE_EQUAL(reported, row["reported"].as_uint64());
      BOOST_REQUIRE_EQUAL(expected, row["expected"].as_uint64());
      BOOST_REQUIRE(last_trace != nullptr);
      BOOST_REQUIRE_EQUAL(time_point_sec(last_trace->block_time.to_time_point()), row["at"].as<time_point_sec>());
   }
   /// Pull the cord as sysio, keeping the trace in `last_trace`.
   action_result andon_pull() {
      return push(sysio_system::test_support::andon::account, andon_abi_ser, SYSIO_ACCOUNT, "pull"_n,
                  mvo()("actor", SYSIO_ACCOUNT)("reason", "incident"));
   }
   /// Clear the cord as sysio, keeping the trace in `last_trace`.
   action_result andon_clear() {
      return push(sysio_system::test_support::andon::account, andon_abi_ser, SYSIO_ACCOUNT, "clear"_n,
                  mvo()("actor", SYSIO_ACCOUNT)("note", "resolved"));
   }
   /// The refusal of a frozen signed action.
   static action_result frozen() { return wasm_assert_msg(sysio_system::test_support::andon::frozen_message); }
   /// `challenger` challenges the envelope of `(chain_code, token_code, epoch)`, signed by `signer`
   /// (the challenger by default).
   action_result challenge(name challenger, std::string_view chain_code, std::string_view token_code, uint32_t epoch,
                           name signer = name{}) {
      return push(SYND_ACCOUNT, synd_abi_ser, signer == name{} ? challenger : signer, "challenge"_n, mvo()
         ("challenger", challenger)("chain_code", codename_mvo(chain_code))("token_code", codename_mvo(token_code))
         ("epoch_index", epoch));
   }
   /// Hold `amount` of `pubkey`'s syndication of `token_code` in a one-item envelope of SOLANA at `epoch`,
   /// and close it. `sequence` must be past the outpost's cursor.
   void syndicate_and_close(uint32_t epoch, uint64_t sequence, const std::vector<char>& pubkey,
                            std::string_view token_code, uint64_t amount) {
      const auto digest = digest_of("envelope-" + std::to_string(epoch));
      BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, epoch, digest, sequence, ChainKind::CHAIN_KIND_SVM, pubkey,
                                            token_code, amount));
      BOOST_REQUIRE_EQUAL(success(), closeenv(SOLANA, epoch, digest));
   }
   /// The statement sysio.synd registers for one token of one envelope: the chain code and token code as
   /// 8-byte little-endian words, the epoch as a 4-byte one and the digest's 32 raw bytes, in the order
   /// chain, epoch, digest, token.
   static std::vector<char> statement_of(std::string_view chain_code, uint32_t epoch, const fc::sha256& digest,
                                         std::string_view token_code) {
      std::vector<char> out;
      const auto        append_le = [&](uint64_t value, size_t bytes) {
         for (size_t i = 0; i < bytes; ++i) out.push_back(char((value >> (8 * i)) & 0xff));
      };
      append_le(slug_value(chain_code), sizeof(uint64_t));
      append_le(epoch, sizeof(uint32_t));
      out.insert(out.end(), digest.data(), digest.data() + digest.data_size());
      append_le(slug_value(token_code), sizeof(uint64_t));
      return out;
   }

   // --- sysio.bond ---

   fc::variant bond_request(uint64_t id) {
      return decode(bond_abi_ser, "request_row", get_row_by_id(BOND_ACCOUNT, BOND_ACCOUNT, "requests"_n, id));
   }
   uint64_t next_bond_request_id() {
      const auto row = decode(bond_abi_ser, "bond_counters",
                              get_row_by_account(BOND_ACCOUNT, BOND_ACCOUNT, "bondcounters"_n, "bondcounters"_n));
      return row.is_null() ? 1 : row["next_request_id"].as_uint64();
   }
   /// Bond `amount` of request `request_id` as `underwriter`, minting it the shadow first.
   action_result underwrite(name underwriter, uint64_t request_id, uint64_t amount, std::string_view token_code) {
      const auto minted = liq_mint(underwriter, token_code, amount);
      if (minted != success()) return minted;
      return push(BOND_ACCOUNT, bond_abi_ser, underwriter, "accept"_n, mvo()
         ("underwriter", underwriter)("request_id", request_id)("amount", amount));
   }
   /// Rule request `id` as sysio: VALID when `valid`, INVALID otherwise.
   action_result rule(uint64_t id, bool valid) {
      return push(BOND_ACCOUNT, bond_abi_ser, SYSIO_ACCOUNT, valid ? "rslvvalid"_n : "rslvinvalid"_n,
                  mvo()("request_id", id));
   }
   /// sysio.bond's permissionless `prune`, signed by carol.
   action_result bond_prune(uint64_t from_id, uint32_t limit) {
      return push(BOND_ACCOUNT, bond_abi_ser, "carol"_n, "prune"_n, mvo()("from_id", from_id)("limit", limit));
   }
   /// sysio.bond's `hold` of request `id` naming `beneficiary`, signed by `signer` (the issuer, sysio.synd).
   action_result bond_hold(uint64_t id, name beneficiary, name signer = SYND_ACCOUNT) {
      return push(BOND_ACCOUNT, bond_abi_ser, signer, "hold"_n, mvo()("request_id", id)("beneficiary", beneficiary));
   }
   /// `account`'s claim on request `id`, signed by it.
   action_result bond_claim(uint64_t id, name account) {
      return push(BOND_ACCOUNT, bond_abi_ser, account, "claim"_n, mvo()("request_id", id)("account", account));
   }
   /// Bond the whole of the request of envelope `(chain_code, token_code, epoch)` as dave.
   void underwrite_envelope(std::string_view chain_code, std::string_view token_code, uint32_t epoch) {
      const auto id      = envelope_row(chain_code, token_code, epoch)["request_id"].as_uint64();
      const auto covered = bond_request(id)["covered"].as_uint64();
      BOOST_REQUIRE_EQUAL(success(), underwrite("dave"_n, id, covered, token_code));
   }

   // --- sysio.liq actions ---

   action_result liq_mint(name to, std::string_view token_code, uint64_t amount, name signer = SYND_ACCOUNT) {
      return push(LIQ_ACCOUNT, liq_abi_ser, signer, "mint"_n, mvo()
         ("to", to)("token_code", codename_mvo(token_code))("amount", amount));
   }
   action_result liq_burn(std::string_view token_code, uint64_t amount, name signer = SYND_ACCOUNT) {
      return push(LIQ_ACCOUNT, liq_abi_ser, signer, "burn"_n, mvo()
         ("token_code", codename_mvo(token_code))("amount", amount));
   }
   action_result liq_mintyield(std::string_view chain_code, std::string_view token_code, uint64_t amount,
                               name signer = SYND_ACCOUNT) {
      return push(LIQ_ACCOUNT, liq_abi_ser, signer, "mintyield"_n, mvo()
         ("chain_code", codename_mvo(chain_code))("token_code", codename_mvo(token_code))("amount", amount));
   }
   action_result transfer_wire(name from, name to, uint64_t amount) {
      return push(TOKEN_ACCOUNT, token_abi_ser, from, "transfer"_n, mvo()
         ("from", from)("to", to)("quantity", asset(static_cast<int64_t>(amount), WIRE_SYM))("memo", ""));
   }
   /// sysio.liq pays `holder` the WIRE its row of `sym` is owed.
   action_result liq_claim(name holder, symbol sym = LIQSOL_SYM) {
      return push(LIQ_ACCOUNT, liq_abi_ser, holder, "claim"_n, mvo()("holder", holder)("sym", sym.to_symbol_code()));
   }
   /// sysio.liq's `creditowed`, signed as sysio.synd unless `signer` says otherwise.
   action_result liq_creditowed(name holder, uint64_t wire, symbol sym = LIQSOL_SYM, name signer = SYND_ACCOUNT) {
      return push(LIQ_ACCOUNT, liq_abi_ser, signer, "creditowed"_n, mvo()
         ("holder", holder)("sym", sym.to_symbol_code())("wire", wire));
   }
   action_result addyield(name from, uint64_t amount, symbol target = LIQSOL_SYM) {
      return push(LIQ_ACCOUNT, liq_abi_ser, from, "addyield"_n, mvo()
         ("from", from)("quantity", asset(static_cast<int64_t>(amount), WIRE_SYM))("target", target.to_symbol_code()));
   }
   action_result create_pool(std::string_view chain_code, std::string_view token_code, symbol pair_symbol,
                             uint64_t initial_chain_amount, uint64_t initial_wire_amount, int32_t fee,
                             int64_t locked_shares, uint32_t horizon_sec, uint32_t depth_cap_bps, int64_t clip_floor) {
      return push(LIQ_ACCOUNT, liq_abi_ser, LIQ_ACCOUNT, "regliqpool"_n, mvo()
         ("chain_code", codename_mvo(chain_code))("token_code", codename_mvo(token_code))
         ("pair_symbol", pair_symbol)("initial_chain_amount", initial_chain_amount)
         ("initial_wire_amount", initial_wire_amount)("fee", fee)("locked_shares", locked_shares)
         ("conversion_horizon_sec", horizon_sec)("depth_cap_bps", depth_cap_bps)("clip_floor", clip_floor));
   }

   // --- rows ---

   fc::variant decode(abi_serializer& ser, const char* type, const std::vector<char>& data) {
      return data.empty()
                ? fc::variant()
                : ser.binary_to_variant(type, data, abi_serializer::create_yield_function(abi_serializer_max_time));
   }
   /// Append `value` to a kv key as the big-endian word the CDT key serializer writes.
   static void append_key_word(std::string& key, uint64_t value) {
      for (int shift = 56; shift >= 0; shift -= 8) key.push_back(char((value >> shift) & 0xff));
   }
   /// sysio.synd's kv rows of `table` in key order whose key starts with `prefix`, as raw values.
   std::vector<std::vector<char>> kv_rows(name table, const std::string& prefix = {}) {
      const auto&                    kv_idx   = control->db().get_index<kv_index, by_code_key>();
      const auto                     table_id = compute_table_id(table.to_uint64_t());
      std::vector<std::vector<char>> rows;
      for (auto itr = kv_idx.lower_bound(boost::make_tuple(SYND_ACCOUNT, table_id, std::string_view(prefix)));
           itr != kv_idx.end() && itr->code == SYND_ACCOUNT && itr->table_id == table_id &&
           itr->key_view().starts_with(prefix);
           ++itr)
         rows.emplace_back(itr->value.data(), itr->value.data() + itr->value.size());
      return rows;
   }
   /// The `envelopes` row of `(chain_code, token_code, epoch)`; null when there is none.
   fc::variant envelope_row(std::string_view chain_code, std::string_view token_code, uint32_t epoch) {
      std::string prefix;
      append_key_word(prefix, slug_value(chain_code));
      append_key_word(prefix, slug_value(token_code));
      for (const auto& value : kv_rows("envelopes"_n, prefix)) {
         const auto row = decode(synd_abi_ser, "envelope_row", value);
         if (row["epoch_index"].as<uint32_t>() == epoch) return row;
      }
      return fc::variant();
   }
   /// The `items` row of `id`; null when there is none. Items are keyed by their envelope first.
   fc::variant item_row(uint64_t id) {
      for (const auto& value : kv_rows("items"_n)) {
         const auto row = decode(synd_abi_ser, "item_row", value);
         if (row["id"].as_uint64() == id) return row;
      }
      return fc::variant();
   }
   fc::variant cursor_row(std::string_view chain_code) {
      return decode(synd_abi_ser, "synd_cursor",
                    get_row_by_id(SYND_ACCOUNT, SYND_ACCOUNT, "syndcursors"_n, slug_value(chain_code)));
   }
   fc::variant ledger_row(std::string_view chain_code, std::string_view token_code) {
      std::string key;
      append_key_word(key, slug_value(chain_code));
      append_key_word(key, slug_value(token_code));
      const auto rows = kv_rows("ledger"_n, key);
      return rows.empty() ? fc::variant() : decode(synd_abi_ser, "ledger_row", rows.front());
   }
   /// Rows in every sysio.synd table: zero means the contract holds no state at all.
   size_t synd_row_count() {
      size_t count = 0;
      for (auto table : { "envelopes"_n, "items"_n, "ledger"_n, "syndcursors"_n, "yieldpool"_n })
         count += kv_rows(table).size();
      return count + get_row_by_account(SYND_ACCOUNT, SYND_ACCOUNT, "syndcounters"_n, "syndcounters"_n).size();
   }
   fc::variant liq_row(name table, const char* type, name scope, uint64_t id) {
      return decode(liq_abi_ser, type, get_row_by_id(LIQ_ACCOUNT, scope, table, id));
   }
   int64_t liq_balance(name holder, symbol sym = LIQSOL_SYM) {
      const auto row = liq_row("accounts"_n, "account", holder, sym.to_symbol_code().value);
      return row.is_null() ? 0 : row["balance"].as<asset>().get_amount();
   }
   int64_t supply(symbol sym = LIQSOL_SYM) {
      return liq_row("stat"_n, "currency_stats", LIQ_ACCOUNT, sym.to_symbol_code().value)["supply"].as<asset>()
         .get_amount();
   }
   int64_t pending(symbol sym = LIQSOL_SYM) {
      const auto row = liq_row("liqpending"_n, "pending_yield", LIQ_ACCOUNT, sym.to_symbol_code().value);
      return row.is_null() ? 0 : row["quantity"].as<asset>().get_amount();
   }
   fc::uint128_t liq_index(symbol sym = LIQSOL_SYM) {
      const auto row = liq_row("yieldidx"_n, "yield_index", LIQ_ACCOUNT, sym.to_symbol_code().value);
      return row.is_null() ? fc::uint128_t{0} : row["index"].as_uint128();
   }
   /// What `holder`'s sysio.liq row of `sym` is owed now, by the token's own formula.
   int64_t liq_owed(name holder, symbol sym = LIQSOL_SYM) {
      const auto row = liq_row("accounts"_n, "account", holder, sym.to_symbol_code().value);
      if (row.is_null()) return 0;
      return yield_reference::owed(row["balance"].as<asset>().get_amount(), liq_index(sym),
                                   row["index_checkpoint"].as_uint128(), row["owed_wire"].as_uint64());
   }
   /// The `parked` row of `(token_code, kind, pubkey)`; null when there is none. The key is the token and
   /// the chain kind as big-endian words, then the pubkey NUL-escaped and NUL-NUL terminated (the kv key
   /// encoding of the contract's `parked_key`).
   fc::variant parked_row(std::string_view token_code, ChainKind kind, const std::vector<char>& pubkey) {
      std::string key;
      append_key_word(key, slug_value(token_code));
      append_key_word(key, static_cast<uint64_t>(magic_enum::enum_integer(kind)));
      for (char c : pubkey) {
         key.push_back(c);
         if (c == '\0') key.push_back('\x01');
      }
      key.push_back('\0');
      key.push_back('\0');
      const auto& kv_idx = control->db().get_index<kv_index, by_code_key>();
      const auto  itr    = kv_idx.find(boost::make_tuple(SYND_ACCOUNT, compute_table_id("parked"_n.to_uint64_t()),
                                                         std::string_view(key)));
      if (itr == kv_idx.end()) return fc::variant();
      return decode(synd_abi_ser, "parked_row",
                    std::vector<char>(itr->value.data(), itr->value.data() + itr->value.size()));
   }
   uint64_t parked_balance(std::string_view token_code, ChainKind kind, const std::vector<char>& pubkey) {
      const auto row = parked_row(token_code, kind, pubkey);
      return row.is_null() ? 0 : row["balance"].as_uint64();
   }
   fc::variant return_row(uint64_t request_id) {
      return decode(synd_abi_ser, "return_row", get_row_by_id(SYND_ACCOUNT, SYND_ACCOUNT, "returns"_n, request_id));
   }
   fc::variant pool_row(std::string_view token_code) {
      return decode(synd_abi_ser, "pool_row",
                    get_row_by_id(SYND_ACCOUNT, SYND_ACCOUNT, "yieldpool"_n, slug_value(token_code)));
   }
   fc::variant state_row() {
      return decode(synd_abi_ser, "synd_state",
                    get_row_by_account(SYND_ACCOUNT, SYND_ACCOUNT, "syndstate"_n, "syndstate"_n));
   }
   /// The `syndconfig` row of `(chain_code, token_code)`; null when there is none.
   fc::variant config_row(std::string_view chain_code, std::string_view token_code) {
      std::string key;
      append_key_word(key, slug_value(chain_code));
      append_key_word(key, slug_value(token_code));
      const auto rows = kv_rows("syndconfig"_n, key);
      return rows.empty() ? fc::variant() : decode(synd_abi_ser, "synd_config", rows.front());
   }
   /// The `buckets` row of `(chain_code, token_code)` for the direction whose one-byte enum value is
   /// `direction`; null when there is none.
   fc::variant bucket_row(std::string_view chain_code, std::string_view token_code, char direction) {
      std::string key;
      append_key_word(key, slug_value(chain_code));
      append_key_word(key, slug_value(token_code));
      key.push_back(direction);
      const auto rows = kv_rows("buckets"_n, key);
      return rows.empty() ? fc::variant() : decode(synd_abi_ser, "bucket_row", rows.front());
   }
   /// The syndication `buckets` row of `(chain_code, token_code)`; null when there is none.
   fc::variant synd_bucket_row(std::string_view chain_code, std::string_view token_code) {
      return bucket_row(chain_code, token_code, SYNDICATION_DIRECTION);
   }
   /// The desyndication `buckets` row of `(chain_code, token_code)`; null when there is none.
   fc::variant desynd_bucket_row(std::string_view chain_code, std::string_view token_code) {
      return bucket_row(chain_code, token_code, DESYNDICATION_DIRECTION);
   }
   /// Shadow base units in the `feepot` of `token_code`.
   /// The `feepot` row of `token_code`; null when there is none.
   fc::variant feepot_row(std::string_view token_code) {
      return decode(synd_abi_ser, "fee_row",
                    get_row_by_id(SYND_ACCOUNT, SYND_ACCOUNT, "feepot"_n, slug_value(token_code)));
   }
   uint64_t feepot_balance(std::string_view token_code) {
      const auto row = feepot_row(token_code);
      return row.is_null() ? 0 : row["balance"].as_uint64();
   }
   fc::variant counters_row() {
      return decode(synd_abi_ser, "synd_counters",
                    get_row_by_account(SYND_ACCOUNT, SYND_ACCOUNT, "syndcounters"_n, "syndcounters"_n));
   }
   fc::variant attestation_row(uint64_t id) {
      return decode(msgch_abi_ser, "attestation_entry",
                    get_row_by_id(MSGCH_ACCOUNT, MSGCH_ACCOUNT, "attestations"_n, id));
   }
   fc::variant swap_pool_row(symbol pair) {
      return decode(swap_abi_ser, "currency_stats",
                    get_row_by_id(SWAP_ACCOUNT, SWAP_ACCOUNT, "stat"_n, pair.to_symbol_code().value));
   }
   int64_t wire_balance(name holder) {
      const auto row = decode(token_abi_ser, "account",
                              get_row_by_id(TOKEN_ACCOUNT, holder, "accounts"_n, WIRE_SYM.to_symbol_code().value));
      return row.is_null() ? 0 : row["balance"].as<asset>().get_amount();
   }
   /// What held item `id` is owed now: its position settled at the live index over what it holds.
   int64_t item_owed(uint64_t id, symbol sym = LIQSOL_SYM) {
      const auto row = item_row(id);
      return yield_reference::owed(static_cast<int64_t>(row["remaining"].as_uint64()), liq_index(sym),
                                   row["position"]["index_checkpoint"].as_uint128(),
                                   row["position"]["owed_wire"].as_uint64());
   }

   /// Whether the last pushed transaction executed `code::action_name` (as its own receiver).
   bool executed(name code, name action_name) const {
      if (!last_trace) return false;
      return std::any_of(last_trace->action_traces.begin(), last_trace->action_traces.end(), [&](const auto& at) {
         return at.act.account == code && at.act.name == action_name && at.receiver == code;
      });
   }

   abi_serializer synd_abi_ser, liq_abi_ser, token_abi_ser, tokens_abi_ser, chains_abi_ser, authex_abi_ser,
      dclaim_abi_ser, msgch_abi_ser, swap_abi_ser, bond_abi_ser, epoch_abi_ser, andon_abi_ser;
   transaction_trace_ptr last_trace;
};

/// The contracts deployed and WIRE issued, but no registry row and no shadow: the state the launch
/// bootstrap finds when it replays a config.
struct sysio_synd_config_tester : sysio_synd_tester {
   sysio_synd_config_tester() : sysio_synd_tester(false) {}
};

/// The registries seeded, and sysio.andon deployed with no puller registered.
struct sysio_synd_unregistered_andon_tester : sysio_synd_tester {
   sysio_synd_unregistered_andon_tester() : sysio_synd_tester(true, andon_setup::unregistered) {}
};

/// The registries seeded, and no sysio.andon at all.
struct sysio_synd_no_andon_tester : sysio_synd_tester {
   sysio_synd_no_andon_tester() : sysio_synd_tester(true, andon_setup::absent) {}
};

BOOST_AUTO_TEST_SUITE(sysio_synd_tests)

// ---------------------------------------------------------------------------
// Intake: SYNDICATE_LIQ
// ---------------------------------------------------------------------------

// A linked and an unlinked pubkey are held the same way: each becomes a SYNDICATION item for its
// pubkey, the shadow is minted into sysio.synd's own holder row, and no account is credited -- not
// even the one the linked pubkey belongs to.
BOOST_FIXTURE_TEST_CASE(onsynd_holds_linked_and_unlinked_alike, sysio_synd_tester) try {
   const auto linked_key = ed_key();
   const auto linked     = ed_bytes(linked_key);
   const auto unlinked   = ed_bytes(ed_key());
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, linked_key));
   const auto digest = digest_of("envelope-1");

   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 1, ChainKind::CHAIN_KIND_SVM, linked, LIQSOL, 40 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 2, ChainKind::CHAIN_KIND_SVM, unlinked, LIQSOL, 60 * UNIT));

   const auto first  = item_row(1);
   const auto second = item_row(2);
   BOOST_REQUIRE(!first.is_null());
   BOOST_REQUIRE(!second.is_null());
   BOOST_REQUIRE_EQUAL(KIND_SYNDICATION, first["kind"].as_string());
   BOOST_REQUIRE_EQUAL(KIND_SYNDICATION, second["kind"].as_string());
   BOOST_REQUIRE(linked == first["pubkey"].as<std::vector<char>>());
   BOOST_REQUIRE(unlinked == second["pubkey"].as<std::vector<char>>());
   BOOST_REQUIRE_EQUAL("CHAIN_KIND_SVM", first["chain_kind"].as_string());
   BOOST_REQUIRE_EQUAL(SOLANA, first["chain_code"].as_string());
   BOOST_REQUIRE_EQUAL(LIQSOL, first["token_code"].as_string());
   BOOST_REQUIRE_EQUAL(1u, first["epoch_index"].as<uint32_t>());
   BOOST_REQUIRE_EQUAL(40 * UNIT, first["amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(40 * UNIT, first["remaining"].as_uint64());
   BOOST_REQUIRE_EQUAL(60 * UNIT, second["remaining"].as_uint64());

   // The shadow sits in sysio.synd's own row; the linked account holds none of it.
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(100 * UNIT), liq_balance(SYND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(100 * UNIT), supply());
   BOOST_REQUIRE_EQUAL(0, liq_balance("alice"_n));

   const auto envelope = envelope_row(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE(!envelope.is_null());
   BOOST_REQUIRE_EQUAL(STATE_OPEN, envelope["state"].as_string());
   BOOST_REQUIRE_EQUAL(100 * UNIT, envelope["synd_total"].as_uint64());
   BOOST_REQUIRE_EQUAL(2u, envelope["item_count"].as<uint32_t>());
   BOOST_REQUIRE_EQUAL(digest, envelope["digest"].as<fc::sha256>());
   BOOST_REQUIRE_EQUAL(2u, cursor_row(SOLANA)["last_sequence"].as_uint64());

   BOOST_REQUIRE(mentions(onsynd(SOLANA, 1, digest, 3, ChainKind::CHAIN_KIND_SVM, linked, LIQSOL, UNIT, "alice"_n),
                          "missing authority of sysio.msgch"));
} FC_LOG_AND_RETHROW()

// A replayed or earlier sequence is recorded and dropped: the action succeeds, nothing is held, and
// the cursor stays where it was. A later sequence, gaps included, is admitted.
BOOST_FIXTURE_TEST_CASE(onsynd_refuses_a_replayed_sequence, sysio_synd_tester) try {
   const auto pubkey = ed_bytes(ed_key());
   const auto digest = digest_of("envelope-1");
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 5, ChainKind::CHAIN_KIND_SVM, pubkey, LIQSOL, UNIT));

   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 5, ChainKind::CHAIN_KIND_SVM, pubkey, LIQSOL, 2 * UNIT));
   BOOST_REQUIRE(console_has(DROP_REPLAY));
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 4, ChainKind::CHAIN_KIND_SVM, pubkey, LIQSOL, 2 * UNIT));
   BOOST_REQUIRE(console_has(DROP_REPLAY));
   // LIQ_YIELD shares the sequence.
   BOOST_REQUIRE_EQUAL(success(), onyield(SOLANA, 1, digest, 5, 9, LIQSOL, UNIT));
   BOOST_REQUIRE(console_has(DROP_YIELD_REPLAY));

   BOOST_REQUIRE(item_row(2).is_null());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(UNIT), supply());
   BOOST_REQUIRE_EQUAL(5u, cursor_row(SOLANA)["last_sequence"].as_uint64());
   BOOST_REQUIRE_EQUAL(1u, envelope_row(SOLANA, LIQSOL, 1)["item_count"].as<uint32_t>());

   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 9, ChainKind::CHAIN_KIND_SVM, pubkey, LIQSOL, 2 * UNIT));
   BOOST_REQUIRE_EQUAL(2 * UNIT, item_row(2)["amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(9u, cursor_row(SOLANA)["last_sequence"].as_uint64());
   // The cursor is per outpost: another outpost starts from nothing.
   BOOST_REQUIRE(cursor_row(ETH).is_null());
} FC_LOG_AND_RETHROW()

// Every message sysio.synd cannot hold is dropped with its reason and consumes no sequence: a liq
// token without a shadow, a token of another outpost, a pubkey that does not fit the chain family,
// a chain family that is not the outpost's, an amount out of range or past the shadow's headroom.
BOOST_FIXTURE_TEST_CASE(onsynd_drops_what_it_cannot_hold_without_consuming_a_sequence, sysio_synd_tester) try {
   const auto pubkey = ed_bytes(ed_key());
   const auto digest = digest_of("envelope-1");
   constexpr auto SVM = ChainKind::CHAIN_KIND_SVM;

   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 1, SVM, pubkey, LIQNONE, UNIT));
   BOOST_REQUIRE(console_has(DROP_NO_SHADOW));
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 1, SVM, pubkey, "NOPE", UNIT));
   BOOST_REQUIRE(console_has(DROP_NO_SHADOW));
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 1, SVM, pubkey, LIQETH, UNIT));
   BOOST_REQUIRE(console_has(DROP_OTHER_CHAIN));
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 1, SVM, std::vector<char>(20, 'x'), LIQSOL, UNIT));
   BOOST_REQUIRE(console_has(DROP_PUBKEY));
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 1, ChainKind::CHAIN_KIND_EVM, em_bytes(em_key()), LIQSOL,
                                         UNIT));
   BOOST_REQUIRE(console_has(DROP_KIND));
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 1, SVM, pubkey, LIQSOL, 0));
   BOOST_REQUIRE(console_has(DROP_RANGE));
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 1, SVM, pubkey, LIQSOL, DEPOT_AMOUNT_MAX + 1));
   BOOST_REQUIRE(console_has(DROP_RANGE));

   // Past the headroom: supply is taken to 10 base units under the asset range first.
   const uint64_t range = static_cast<uint64_t>(asset::max_amount);
   BOOST_REQUIRE_EQUAL(success(), liq_mint("bob"_n, LIQSOL, range - 10));
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 1, SVM, pubkey, LIQSOL, 11));
   BOOST_REQUIRE(console_has(DROP_HEADROOM));

   // Nothing was held and no sequence was consumed: the same sequence still admits a message.
   BOOST_REQUIRE(item_row(1).is_null());
   BOOST_REQUIRE(envelope_row(SOLANA, LIQSOL, 1).is_null());
   BOOST_REQUIRE(cursor_row(SOLANA).is_null());
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 1, SVM, pubkey, LIQSOL, 10));
   BOOST_REQUIRE_EQUAL(10u, item_row(1)["amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(1u, cursor_row(SOLANA)["last_sequence"].as_uint64());
   BOOST_REQUIRE_EQUAL(asset::max_amount, supply());
} FC_LOG_AND_RETHROW()

// A message that would leave its envelope impossible to underwrite is dropped at intake, before it
// consumes a sequence: a total that, rounded up to the bond increment, passes the asset range (the
// request's covered amount could never be carried), and a yield total past the shadow's headroom.
BOOST_FIXTURE_TEST_CASE(intake_drops_what_would_make_the_envelope_unrequestable, sysio_synd_tester) try {
   const auto         pubkey    = ed_bytes(ed_key());
   const auto         digest    = digest_of("envelope-1");
   constexpr auto     SVM       = ChainKind::CHAIN_KIND_SVM;
   constexpr uint64_t INCREMENT = 10'000'000;   // 0.01 of a precision-9 token: sysio.bond's increment
   const uint64_t     range     = static_cast<uint64_t>(asset::max_amount);

   // The whole asset range as one yield report fits the headroom, but rounds up past the range.
   BOOST_REQUIRE_EQUAL(success(), onyield(SOLANA, 1, digest, 1, 7, LIQSOL, range));
   BOOST_REQUIRE(console_has(DROP_YIELD_ENVELOPE_RANGE));
   BOOST_REQUIRE(envelope_row(SOLANA, LIQSOL, 1).is_null());
   BOOST_REQUIRE(cursor_row(SOLANA).is_null());

   // The largest multiple of the increment is held; a syndication on top of it would pass the range.
   const uint64_t largest = range / INCREMENT * INCREMENT;
   BOOST_REQUIRE_EQUAL(success(), onyield(SOLANA, 1, digest, 1, 7, LIQSOL, largest));
   BOOST_REQUIRE_EQUAL(largest, envelope_row(SOLANA, LIQSOL, 1)["yield_total"].as_uint64());
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 2, SVM, pubkey, LIQSOL, UNIT));
   BOOST_REQUIRE(console_has(DROP_ENVELOPE_RANGE));
   BOOST_REQUIRE_EQUAL(0, supply());
   BOOST_REQUIRE_EQUAL(1u, cursor_row(SOLANA)["last_sequence"].as_uint64());

   // Another envelope: with the supply 10 UNIT under the range, 6 UNIT of yield fit and 5 more do not.
   const auto digest2 = digest_of("envelope-2");
   BOOST_REQUIRE_EQUAL(success(), liq_mint("bob"_n, LIQSOL, range - 10 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), onyield(SOLANA, 2, digest2, 2, 8, LIQSOL, 6 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), onyield(SOLANA, 2, digest2, 3, 8, LIQSOL, 5 * UNIT));
   BOOST_REQUIRE(console_has(DROP_YIELD_HEADROOM));
   BOOST_REQUIRE_EQUAL(6 * UNIT, envelope_row(SOLANA, LIQSOL, 2)["yield_total"].as_uint64());
   BOOST_REQUIRE_EQUAL(2u, cursor_row(SOLANA)["last_sequence"].as_uint64());
   // A syndication still fits the envelope, so long as the supply has room for it.
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 2, digest2, 3, SVM, pubkey, LIQSOL, 4 * UNIT));
   BOOST_REQUIRE_EQUAL(4 * UNIT, envelope_row(SOLANA, LIQSOL, 2)["synd_total"].as_uint64());
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Intake: LIQ_YIELD
// ---------------------------------------------------------------------------

// A yield report is held as a number: a YIELD item and the envelope's yield total grow, while the
// supply, the pending yield and sysio.synd's holder row stay where they were.
BOOST_FIXTURE_TEST_CASE(onyield_holds_the_amount_without_minting, sysio_synd_tester) try {
   const auto digest = digest_of("envelope-1");
   BOOST_REQUIRE_EQUAL(success(), onyield(SOLANA, 1, digest, 1, 42, LIQSOL, 7 * UNIT));

   const auto item = item_row(1);
   BOOST_REQUIRE(!item.is_null());
   BOOST_REQUIRE_EQUAL(KIND_YIELD, item["kind"].as_string());
   BOOST_REQUIRE_EQUAL("CHAIN_KIND_UNKNOWN", item["chain_kind"].as_string());
   BOOST_REQUIRE(item["pubkey"].as<std::vector<char>>().empty());
   BOOST_REQUIRE_EQUAL(7 * UNIT, item["amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(7 * UNIT, item["remaining"].as_uint64());

   BOOST_REQUIRE_EQUAL(0, supply());
   BOOST_REQUIRE_EQUAL(0, pending());
   BOOST_REQUIRE_EQUAL(0, liq_balance(SYND_ACCOUNT));

   const auto envelope = envelope_row(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(7 * UNIT, envelope["yield_total"].as_uint64());
   BOOST_REQUIRE_EQUAL(0u, envelope["synd_total"].as_uint64());
   const auto cursor = cursor_row(SOLANA);
   BOOST_REQUIRE_EQUAL(1u, cursor["last_sequence"].as_uint64());
   BOOST_REQUIRE_EQUAL(42u, cursor["last_epoch"].as_uint64());

   // A syndication after it keeps the outpost epoch the yield recorded.
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 2, ChainKind::CHAIN_KIND_SVM, ed_bytes(ed_key()), LIQSOL,
                                         UNIT));
   BOOST_REQUIRE_EQUAL(42u, cursor_row(SOLANA)["last_epoch"].as_uint64());
   BOOST_REQUIRE(mentions(onyield(SOLANA, 1, digest, 3, 43, LIQSOL, UNIT, "alice"_n),
                          "missing authority of sysio.msgch"));
} FC_LOG_AND_RETHROW()

// A yield report holds no shadow, yet its position checkpoints the live index at intake: settled by
// mistake it would owe nothing, never the index's whole history.
BOOST_FIXTURE_TEST_CASE(a_yield_item_checkpoints_the_live_index, sysio_synd_tester) try {
   BOOST_REQUIRE_EQUAL(success(), liq_mint("bob"_n, LIQSOL, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 3 * UNIT));
   BOOST_REQUIRE(liq_index() > 0);

   BOOST_REQUIRE_EQUAL(success(), onyield(SOLANA, 1, digest_of("envelope-1"), 1, 42, LIQSOL, 7 * UNIT));
   const auto position = item_row(1)["position"];
   BOOST_REQUIRE(position["index_checkpoint"].as_uint128() == liq_index());
   BOOST_REQUIRE_EQUAL(0u, position["owed_wire"].as_uint64());
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Closing an envelope
// ---------------------------------------------------------------------------

// `closeenv` closes every OPEN row of the outpost's envelope, one per token, with the totals intake
// recorded -- and the queue step it runs inline asks sysio.bond to underwrite each of them at once --
// and leaves other epochs and other outposts alone. A message arriving for a closed envelope is dropped
// without consuming its sequence.
BOOST_FIXTURE_TEST_CASE(closeenv_closes_the_envelope_and_records_totals, sysio_synd_tester) try {
   const auto digest = digest_of("envelope-3");
   const auto pubkey = ed_bytes(ed_key());
   constexpr auto SVM = ChainKind::CHAIN_KIND_SVM;
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 3, digest, 1, SVM, pubkey, LIQSOL, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 3, digest, 2, SVM, pubkey, LIQSOL, 5 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), onyield(SOLANA, 3, digest, 3, 17, LIQSOL, 2 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 3, digest, 4, SVM, pubkey, LIQTWO, 8 * UNIT));
   // Another epoch of the same outpost, and another outpost's envelope of the same epoch.
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 4, digest_of("envelope-4"), 5, SVM, pubkey, LIQSOL, UNIT));
   BOOST_REQUIRE_EQUAL(success(), onyield(ETH, 3, digest_of("eth-3"), 1, 1, LIQETH, UNIT));

   // A wrong digest closes nothing.
   BOOST_REQUIRE_EQUAL(success(), closeenv(SOLANA, 3, digest_of("forged")));
   BOOST_REQUIRE(console_has(DROP_DIGEST));
   BOOST_REQUIRE_EQUAL(STATE_OPEN, envelope_row(SOLANA, LIQSOL, 3)["state"].as_string());

   BOOST_REQUIRE_EQUAL(success(), closeenv(SOLANA, 3, digest));
   const auto liqsol = envelope_row(SOLANA, LIQSOL, 3);
   BOOST_REQUIRE_EQUAL(STATE_REQUESTED, liqsol["state"].as_string());
   BOOST_REQUIRE_EQUAL(15 * UNIT, liqsol["synd_total"].as_uint64());
   BOOST_REQUIRE_EQUAL(2 * UNIT, liqsol["yield_total"].as_uint64());
   BOOST_REQUIRE_EQUAL(3u, liqsol["item_count"].as<uint32_t>());
   BOOST_REQUIRE_EQUAL(1u, liqsol["request_id"].as_uint64());
   BOOST_REQUIRE_EQUAL(0u, liqsol["released"].as_uint64());
   BOOST_REQUIRE_EQUAL(0u, liqsol["burned"].as_uint64());
   const auto liqtwo = envelope_row(SOLANA, LIQTWO, 3);
   BOOST_REQUIRE_EQUAL(STATE_REQUESTED, liqtwo["state"].as_string());
   BOOST_REQUIRE_EQUAL(2u, liqtwo["request_id"].as_uint64());
   BOOST_REQUIRE_EQUAL(8 * UNIT, liqtwo["synd_total"].as_uint64());
   BOOST_REQUIRE_EQUAL(1u, liqtwo["item_count"].as<uint32_t>());
   BOOST_REQUIRE_EQUAL(STATE_OPEN, envelope_row(SOLANA, LIQSOL, 4)["state"].as_string());
   BOOST_REQUIRE_EQUAL(STATE_OPEN, envelope_row(ETH, LIQETH, 3)["state"].as_string());

   // The ledger sums every envelope of the outpost and token.
   const auto ledger = ledger_row(SOLANA, LIQSOL);

   // Closed: a late message for it is dropped, its sequence left for the next.
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 3, digest, 6, SVM, pubkey, LIQSOL, UNIT));
   BOOST_REQUIRE(console_has(DROP_CLOSED));
   BOOST_REQUIRE_EQUAL(15 * UNIT, envelope_row(SOLANA, LIQSOL, 3)["synd_total"].as_uint64());
   BOOST_REQUIRE_EQUAL(5u, cursor_row(SOLANA)["last_sequence"].as_uint64());
   // Closing again changes nothing: no second request.
   BOOST_REQUIRE_EQUAL(success(), closeenv(SOLANA, 3, digest));
   BOOST_REQUIRE_EQUAL(STATE_REQUESTED, envelope_row(SOLANA, LIQSOL, 3)["state"].as_string());
   BOOST_REQUIRE_EQUAL(3u, next_bond_request_id());

   BOOST_REQUIRE(mentions(closeenv(SOLANA, 4, digest_of("envelope-4"), "alice"_n), "missing authority of sysio.msgch"));
} FC_LOG_AND_RETHROW()

// Review Focus 3: an envelope that brought no syndication value leaves no trace in sysio.synd.
BOOST_FIXTURE_TEST_CASE(closeenv_without_items_writes_nothing, sysio_synd_tester) try {
   BOOST_REQUIRE_EQUAL(0u, synd_row_count());
   BOOST_REQUIRE_EQUAL(success(), closeenv(SOLANA, 5, digest_of("empty")));
   BOOST_REQUIRE_EQUAL(0u, synd_row_count());
   BOOST_REQUIRE(envelope_row(SOLANA, LIQSOL, 5).is_null());
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Custody of held funds
// ---------------------------------------------------------------------------

// A held item earns the yield its shadow earns in sysio.synd's row: its position is checkpointed at
// the live index at intake, and after a distribution it is owed exactly what the contract's row is.
BOOST_FIXTURE_TEST_CASE(held_items_accrue_yield, sysio_synd_tester) try {
   // Some supply and a first distribution, so the index is not zero at intake.
   BOOST_REQUIRE_EQUAL(success(), liq_mint("bob"_n, LIQSOL, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 10 * UNIT));
   const auto index_at_intake = liq_index();
   BOOST_REQUIRE(index_at_intake > fc::uint128_t{0});

   const auto digest = digest_of("envelope-1");
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 1, ChainKind::CHAIN_KIND_SVM, ed_bytes(ed_key()), LIQSOL,
                                         100 * UNIT));
   const auto item = item_row(1);
   BOOST_REQUIRE_EQUAL(index_at_intake, item["position"]["index_checkpoint"].as_uint128());
   BOOST_REQUIRE_EQUAL(0u, item["position"]["owed_wire"].as_uint64());
   BOOST_REQUIRE_EQUAL(0, item_owed(1));
   BOOST_REQUIRE_EQUAL(0, liq_owed(SYND_ACCOUNT));

   // Half the supply is held: half of the next distribution is the item's.
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 20 * UNIT));
   BOOST_REQUIRE(liq_index() > index_at_intake);
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(10 * UNIT), item_owed(1));
   BOOST_REQUIRE_EQUAL(liq_owed(SYND_ACCOUNT), item_owed(1));
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 4 * UNIT));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(12 * UNIT), item_owed(1));
   BOOST_REQUIRE_EQUAL(liq_owed(SYND_ACCOUNT), item_owed(1));
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// sysio.liq as the ledger
// ---------------------------------------------------------------------------

// Supply is sysio.synd's alone to grow and shrink, and LIQ_YIELD reaches the pending balance only
// through it.
BOOST_FIXTURE_TEST_CASE(liq_mint_and_burn_require_synd, sysio_synd_tester) try {
   BOOST_REQUIRE(mentions(liq_mint("alice"_n, LIQSOL, UNIT, "alice"_n), "missing authority of sysio.synd"));
   BOOST_REQUIRE(mentions(liq_mint("alice"_n, LIQSOL, UNIT, MSGCH_ACCOUNT), "missing authority of sysio.synd"));
   BOOST_REQUIRE(mentions(liq_burn(LIQSOL, UNIT, "alice"_n), "missing authority of sysio.synd"));
   BOOST_REQUIRE(mentions(liq_mintyield(SOLANA, LIQSOL, UNIT, MSGCH_ACCOUNT), "missing authority of sysio.synd"));

   BOOST_REQUIRE_EQUAL(wasm_assert_msg("token_code has no shadow symbol"), liq_mint("alice"_n, LIQNONE, UNIT));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("amount must be positive"), liq_mint("alice"_n, LIQSOL, 0));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("to account does not exist"), liq_mint("nobody"_n, LIQSOL, UNIT));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("supply exceeds the asset range"),
                       liq_mint("alice"_n, LIQSOL, static_cast<uint64_t>(asset::max_amount) + 1));

   BOOST_REQUIRE_EQUAL(success(), liq_mint("alice"_n, LIQSOL, 30 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), liq_mint(SYND_ACCOUNT, LIQSOL, 20 * UNIT));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(30 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(50 * UNIT), supply());

   // Burn settles sysio.synd's row first: what it earned before the burn stays owed.
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 5 * UNIT));
   const int64_t owed_before = liq_owed(SYND_ACCOUNT);
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(2 * UNIT), owed_before);
   BOOST_REQUIRE_EQUAL(success(), liq_burn(LIQSOL, 15 * UNIT));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(5 * UNIT), liq_balance(SYND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(35 * UNIT), supply());
   BOOST_REQUIRE_EQUAL(owed_before, liq_owed(SYND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("overdrawn balance"), liq_burn(LIQSOL, 5 * UNIT + 1));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("amount out of range"), liq_burn(LIQSOL, 0));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("token_code has no shadow symbol"), liq_burn(LIQNONE, UNIT));

   // mintyield never throws: a drop is a diagnostic, a valid report lands in pending.
   BOOST_REQUIRE_EQUAL(success(), liq_mintyield(ETH, LIQSOL, UNIT));
   BOOST_REQUIRE(console_has("sysio.liq::mintyield: DROP -- token_code belongs to another chain"));
   BOOST_REQUIRE_EQUAL(success(), liq_mintyield(SOLANA, LIQSOL, 3 * UNIT));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(3 * UNIT), pending());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(35 * UNIT), supply());
} FC_LOG_AND_RETHROW()

// The syndication entry points and their replay guard are sysio.synd's now: sysio.liq's ABI has no
// `mintsynd`, no `park` and no cursor table, and has `mint` and `burn`.
BOOST_FIXTURE_TEST_CASE(liq_no_longer_has_mintsynd_or_park, sysio_synd_tester) try {
   abi_def abi;
   BOOST_REQUIRE(abi_serializer::to_abi(control->find_account_metadata(LIQ_ACCOUNT)->abi, abi));
   const auto has_action = [&](name n) {
      return std::any_of(abi.actions.begin(), abi.actions.end(), [&](const auto& a) { return a.name == n; });
   };
   const auto has_table = [&](name n) {
      return std::any_of(abi.tables.begin(), abi.tables.end(), [&](const auto& t) { return t.name == n.to_string(); });
   };
   BOOST_REQUIRE(!has_action("mintsynd"_n));
   BOOST_REQUIRE(!has_action("park"_n));
   BOOST_REQUIRE(!has_table("liqcursors"_n));
   BOOST_REQUIRE(has_action("mint"_n));
   BOOST_REQUIRE(has_action("burn"_n));
   BOOST_REQUIRE(has_action("mintyield"_n));
} FC_LOG_AND_RETHROW()

// The moved surface is sysio.synd's alone: sysio.liq's ABI has no parked hold, no sweeps, no
// desyndication, no import and no desyndication counter, and sysio.synd's has all of them.
BOOST_FIXTURE_TEST_CASE(liq_has_none_of_the_moved_actions, sysio_synd_tester) try {
   const auto abi_of = [&](name account) {
      abi_def abi;
      BOOST_REQUIRE(abi_serializer::to_abi(control->find_account_metadata(account)->abi, abi));
      return abi;
   };
   const abi_def liq_abi  = abi_of(LIQ_ACCOUNT);
   const abi_def synd_abi = abi_of(SYND_ACCOUNT);
   const auto has_action = [](const abi_def& abi, name n) {
      return std::any_of(abi.actions.begin(), abi.actions.end(), [&](const auto& a) { return a.name == n; });
   };
   const auto has_table = [](const abi_def& abi, name n) {
      return std::any_of(abi.tables.begin(), abi.tables.end(), [&](const auto& t) { return t.name == n.to_string(); });
   };
   for (auto action : { "sweep"_n, "linkswept"_n, "desyndicate"_n, "importsynd"_n, "importdone"_n }) {
      BOOST_TEST_CONTEXT(action.to_string()) {
         BOOST_REQUIRE(!has_action(liq_abi, action));
         BOOST_REQUIRE(has_action(synd_abi, action));
      }
   }
   BOOST_REQUIRE(!has_table(liq_abi, "parked"_n));
   BOOST_REQUIRE(!has_table(liq_abi, "liqcounters"_n));
   BOOST_REQUIRE(has_table(synd_abi, "parked"_n));
   BOOST_REQUIRE(!has_table(synd_abi, "desyndlog"_n));
   BOOST_REQUIRE(has_table(synd_abi, "returns"_n));
   BOOST_REQUIRE(has_table(synd_abi, "syndconfig"_n));
   BOOST_REQUIRE(has_table(synd_abi, "syndstate"_n));
   BOOST_REQUIRE(has_table(synd_abi, "buckets"_n));
   BOOST_REQUIRE(has_table(synd_abi, "feepot"_n));
   for (auto action : { "setconfig"_n, "crank"_n, "dropenv"_n, "sweepyield"_n })
      BOOST_TEST_CONTEXT(action.to_string()) { BOOST_REQUIRE(has_action(synd_abi, action)); }
   // The import flag left sysio.liq's config with the import.
   const auto liq_config = std::find_if(liq_abi.structs.begin(), liq_abi.structs.end(),
                                        [](const auto& st) { return st.name == "liq_config"; });
   BOOST_REQUIRE(liq_config != liq_abi.structs.end());
   BOOST_REQUIRE(std::none_of(liq_config->fields.begin(), liq_config->fields.end(),
                              [](const auto& f) { return f.name == "import_complete"; }));
   // What stays in the ledger stays.
   for (auto action : { "mint"_n, "burn"_n, "mintyield"_n, "creditowed"_n, "transfer"_n, "claim"_n, "recredit"_n,
                        "regliqpool"_n })
      BOOST_TEST_CONTEXT(action.to_string()) { BOOST_REQUIRE(has_action(liq_abi, action)); }
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// The parked hold
// ---------------------------------------------------------------------------

// A parked row holds its balance in sysio.synd's own sysio.liq row and earns through its position. A
// sweep for an existing link delivers the balance with a sysio.liq transfer and credits the banked WIRE,
// out of the token's yield pool, to the account's own sysio.liq row -- exactly what the row earned,
// claimable there; `linkswept` from sysio.authex does the same and returns quietly on what it cannot
// deliver.
BOOST_FIXTURE_TEST_CASE(parked_rows_accrue_and_are_delivered_on_link, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   BOOST_REQUIRE_EQUAL(success(), liq_mint("bob"_n, LIQSOL, 50 * UNIT));
   // The import parks a position whose pubkey has no link yet.
   BOOST_REQUIRE_EQUAL(success(), importsynd(SOLANA, LIQSOL, { credit(pubkey, 50 * UNIT) }));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(100 * UNIT), supply());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(50 * UNIT), liq_balance(SYND_ACCOUNT));
   const auto parked = parked_row(LIQSOL, ChainKind::CHAIN_KIND_SVM, pubkey);
   BOOST_REQUIRE(!parked.is_null());
   BOOST_REQUIRE_EQUAL(LIQSOL, parked["token_code"].as_string());
   BOOST_REQUIRE(parked["chain_kind"].as<ChainKind>() == ChainKind::CHAIN_KIND_SVM);
   BOOST_REQUIRE(pubkey == parked["pubkey"].as<std::vector<char>>());
   BOOST_REQUIRE_EQUAL(50 * UNIT, parked["balance"].as_uint64());

   // Yield lands while the row is parked: half the supply, half the WIRE.
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(5 * UNIT), liq_owed(SYND_ACCOUNT));

   // Nothing to sweep before the link exists.
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("account has no link for this chain"),
                       sweep("carol"_n, ChainKind::CHAIN_KIND_SVM));
   BOOST_REQUIRE_EQUAL(success(), link_svm("carol"_n, key));
   const int64_t carol_wire = wire_balance("carol"_n);
   BOOST_REQUIRE_EQUAL(success(), sweep("carol"_n, ChainKind::CHAIN_KIND_SVM));
   BOOST_REQUIRE(parked_row(LIQSOL, ChainKind::CHAIN_KIND_SVM, pubkey).is_null());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(50 * UNIT), liq_balance("carol"_n));
   BOOST_REQUIRE_EQUAL(0, liq_balance(SYND_ACCOUNT));
   // The banked WIRE, pulled from sysio.liq into the pool and taken out of it, is credited back to
   // sysio.liq as carol's owed yield: nothing is pushed to her, she claims it.
   BOOST_REQUIRE_EQUAL(carol_wire, wire_balance("carol"_n));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(5 * UNIT), liq_owed("carol"_n));
   BOOST_REQUIRE_EQUAL(0, liq_owed(SYND_ACCOUNT));              // the pull claimed it
   BOOST_REQUIRE_EQUAL(0, wire_balance(SYND_ACCOUNT));          // nothing stranded in the contract
   BOOST_REQUIRE_EQUAL(success(), liq_claim("carol"_n));
   BOOST_REQUIRE_EQUAL(carol_wire + static_cast<int64_t>(5 * UNIT), wire_balance("carol"_n));
   BOOST_REQUIRE_EQUAL(0, liq_owed("carol"_n));
   BOOST_REQUIRE_EQUAL(5 * UNIT, pool_row(LIQSOL)["pool"]["received"].as_uint64());
   BOOST_REQUIRE_EQUAL(5 * UNIT, pool_row(LIQSOL)["pool"]["credited"].as_uint64());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(5 * UNIT), liq_owed("bob"_n));

   // Sweeping again finds nothing.
   BOOST_REQUIRE_EQUAL(success(), sweep("carol"_n, ChainKind::CHAIN_KIND_SVM));
   BOOST_REQUIRE_EQUAL(carol_wire + static_cast<int64_t>(5 * UNIT), wire_balance("carol"_n));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(50 * UNIT), liq_balance("carol"_n));

   // The inline path from sysio.authex delivers the same way; nothing is owed yet, so no WIRE moves.
   const auto key2    = ed_key();
   const auto pubkey2 = ed_bytes(key2);
   BOOST_REQUIRE_EQUAL(success(), importsynd(SOLANA, LIQSOL, { credit(pubkey2, 20 * UNIT) }));
   BOOST_REQUIRE(mentions(linkswept("dave"_n, ChainKind::CHAIN_KIND_SVM, pubkey2, "dave"_n),
                          "missing authority of sysio.authex"));
   // What cannot be delivered returns quietly and leaves the row parked: a pubkey that does not fit
   // the chain family, an account that does not exist, and sysio.synd itself.
   BOOST_REQUIRE_EQUAL(success(), linkswept("dave"_n, ChainKind::CHAIN_KIND_EVM, pubkey2));
   BOOST_REQUIRE_EQUAL(success(), linkswept("nobody"_n, ChainKind::CHAIN_KIND_SVM, pubkey2));
   BOOST_REQUIRE_EQUAL(20 * UNIT, parked_balance(LIQSOL, ChainKind::CHAIN_KIND_SVM, pubkey2));
   const int64_t dave_wire = wire_balance("dave"_n);
   BOOST_REQUIRE_EQUAL(success(), linkswept("dave"_n, ChainKind::CHAIN_KIND_SVM, pubkey2));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(20 * UNIT), liq_balance("dave"_n));
   BOOST_REQUIRE_EQUAL(0, liq_owed("dave"_n));
   BOOST_REQUIRE_EQUAL(dave_wire, wire_balance("dave"_n));
   BOOST_REQUIRE(parked_row(LIQSOL, ChainKind::CHAIN_KIND_SVM, pubkey2).is_null());
} FC_LOG_AND_RETHROW()

// A user-signed `sysio.authex::createlink` sends `linkswept` to sysio.synd, which delivers the shadow
// parked against the linked EM key -- and the WIRE it earned while parked -- in the same transaction.
BOOST_FIXTURE_TEST_CASE(createlink_sweeps_parked_from_synd, sysio_synd_tester) try {
   const auto priv   = fc::crypto::private_key::generate(fc::crypto::private_key::key_type::em);
   const auto pubkey = em_bytes(priv.get_public_key());
   BOOST_REQUIRE_EQUAL(success(), importsynd(ETH, LIQETH, { credit(pubkey, 30 * UNIT) }));
   BOOST_REQUIRE_EQUAL(30 * UNIT, parked_balance(LIQETH, ChainKind::CHAIN_KIND_EVM, pubkey));
   // The parked row is all of LIQETH's supply: every WIRE of this distribution is its.
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 3 * UNIT, LIQETH_SYM));

   const int64_t alice_wire = wire_balance("alice"_n);
   BOOST_REQUIRE_EQUAL(success(), createlink("alice"_n, priv));
   BOOST_REQUIRE(executed(SYND_ACCOUNT, "linkswept"_n));
   BOOST_REQUIRE(parked_row(LIQETH, ChainKind::CHAIN_KIND_EVM, pubkey).is_null());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(30 * UNIT), liq_balance("alice"_n, LIQETH_SYM));
   BOOST_REQUIRE_EQUAL(0, liq_balance(SYND_ACCOUNT, LIQETH_SYM));
   BOOST_REQUIRE_EQUAL(alice_wire, wire_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(3 * UNIT), liq_owed("alice"_n, LIQETH_SYM));
   BOOST_REQUIRE_EQUAL(success(), liq_claim("alice"_n, LIQETH_SYM));
   BOOST_REQUIRE_EQUAL(alice_wire + static_cast<int64_t>(3 * UNIT), wire_balance("alice"_n));
} FC_LOG_AND_RETHROW()

// sysio.liq's `creditowed` is sysio.synd's alone: it moves WIRE from sysio.synd into sysio.liq and
// banks it on the holder's row, which claims it like any yield -- a row it has to create included.
BOOST_FIXTURE_TEST_CASE(liq_creditowed_banks_wire_for_the_holder_and_requires_synd, sysio_synd_tester) try {
   BOOST_REQUIRE_EQUAL(success(), transfer_wire(SYSIO_ACCOUNT, SYND_ACCOUNT, 10 * UNIT));
   BOOST_REQUIRE(mentions(liq_creditowed("dave"_n, UNIT, LIQSOL_SYM, "dave"_n), "missing authority of sysio.synd"));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("amount out of range"), liq_creditowed("dave"_n, 0));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("holder account does not exist"), liq_creditowed("nobody"_n, UNIT));

   const int64_t liq_wire  = wire_balance(LIQ_ACCOUNT);
   const int64_t dave_wire = wire_balance("dave"_n);
   BOOST_REQUIRE_EQUAL(success(), liq_creditowed("dave"_n, 3 * UNIT));
   BOOST_REQUIRE_EQUAL(0, liq_balance("dave"_n));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(3 * UNIT), liq_owed("dave"_n));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(7 * UNIT), wire_balance(SYND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(liq_wire + static_cast<int64_t>(3 * UNIT), wire_balance(LIQ_ACCOUNT));
   BOOST_REQUIRE_EQUAL(dave_wire, wire_balance("dave"_n));

   BOOST_REQUIRE_EQUAL(success(), liq_claim("dave"_n));
   BOOST_REQUIRE_EQUAL(dave_wire + static_cast<int64_t>(3 * UNIT), wire_balance("dave"_n));
   BOOST_REQUIRE_EQUAL(0, liq_owed("dave"_n));
   BOOST_REQUIRE_EQUAL(liq_wire, wire_balance(LIQ_ACCOUNT));
} FC_LOG_AND_RETHROW()

// WIRE a parked row banked that the pool cannot cover at delivery is not lost: the row stays at balance
// 0 with it banked, and a later delivery pays it once the pool covers it. Here sysio.synd's own row is
// claimed around the pool, so the first delivery finds the pool empty.
BOOST_FIXTURE_TEST_CASE(a_parked_delivery_keeps_what_the_pool_cannot_cover, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   const auto other  = ed_bytes(ed_key());
   BOOST_REQUIRE_EQUAL(success(), importsynd(SOLANA, LIQSOL, { credit(pubkey, 50 * UNIT), credit(other, 50 * UNIT) }));
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), liq_claim(SYND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(0, liq_owed(SYND_ACCOUNT));

   BOOST_REQUIRE_EQUAL(success(), link_svm("dave"_n, key));
   BOOST_REQUIRE_EQUAL(success(), sweep("dave"_n, ChainKind::CHAIN_KIND_SVM));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(50 * UNIT), liq_balance("dave"_n));
   BOOST_REQUIRE_EQUAL(0, liq_owed("dave"_n));
   const auto kept = parked_row(LIQSOL, ChainKind::CHAIN_KIND_SVM, pubkey);
   BOOST_REQUIRE(!kept.is_null());
   BOOST_REQUIRE_EQUAL(0u, kept["balance"].as_uint64());
   BOOST_REQUIRE_EQUAL(5 * UNIT, kept["position"]["owed_wire"].as_uint64());

   // Yield the pool receives later covers it: the next sweep pays it and the row goes.
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 20 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), sweep("dave"_n, ChainKind::CHAIN_KIND_SVM));
   BOOST_REQUIRE(parked_row(LIQSOL, ChainKind::CHAIN_KIND_SVM, pubkey).is_null());
   // Dave is owed 10 WIRE as a holder of half the supply, plus the 5 his parked row had banked.
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(15 * UNIT), liq_owed("dave"_n));
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Desyndication
// ---------------------------------------------------------------------------

// A linked holder's shadow moves to sysio.synd and is burned there; DESYNDICATE_LIQ is queued to the
// token's outpost for the holder's linked pubkey with the next request id. The holder keeps the yield
// its row earned before the burn.
BOOST_FIXTURE_TEST_CASE(desyndicate_burns_and_queues_the_attestation, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   BOOST_REQUIRE_EQUAL(success(), liq_mint("alice"_n, LIQSOL, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), liq_mint("bob"_n, LIQSOL, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 20 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {}));

   BOOST_REQUIRE_EQUAL(wasm_assert_msg("holder is not AuthX-linked for the token's chain"),
                       desyndicate("alice"_n, 40 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   BOOST_REQUIRE(mentions(desyndicate("alice"_n, 40 * UNIT, LIQSOL_SYM, "bob"_n), "missing authority of alice"));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("quantity must be positive"), desyndicate("alice"_n, 0));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("shadow symbol does not exist"),
                       desyndicate("alice"_n, UNIT, symbol::from_string("9,NOPE")));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("symbol precision mismatch"),
                       desyndicate("alice"_n, UNIT, symbol::from_string("6,LIQSOL")));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("overdrawn balance"), desyndicate("alice"_n, 101 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), desyndicate("alice"_n, 40 * UNIT));

   // The transfer settled the row first: the yield earned on the whole balance stays.
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(60 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(160 * UNIT), supply());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(10 * UNIT), liq_owed("alice"_n));
   BOOST_REQUIRE_EQUAL(0, liq_balance(SYND_ACCOUNT));   // passed through and burned

   const auto att = attestation_row(1);
   BOOST_REQUIRE(!att.is_null());
   BOOST_REQUIRE_EQUAL("ATTESTATION_TYPE_DESYNDICATE_LIQ", att["type"].as_string());
   BOOST_REQUIRE_EQUAL(slug_value(SOLANA), att["chain_code"].as_uint64());
   const auto data = att["data"].as<std::vector<char>>();
   sysio::opp::attestations::DesyndicateLIQ msg;
   BOOST_REQUIRE(msg.ParseFromArray(data.data(), static_cast<int>(data.size())));
   BOOST_REQUIRE_EQUAL(slug_value(SOLANA), msg.chain_code());
   BOOST_REQUIRE(msg.user().kind() == ChainKind::CHAIN_KIND_SVM);
   BOOST_REQUIRE_EQUAL(std::string(pubkey.begin(), pubkey.end()), msg.user().address());
   BOOST_REQUIRE_EQUAL(slug_value(LIQSOL), msg.amount().token_code());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(40 * UNIT), msg.amount().amount());
   BOOST_REQUIRE_EQUAL(1u, msg.request_id());

   BOOST_REQUIRE_EQUAL(success(), desyndicate("alice"_n, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(3u, counters_row()["next_request_id"].as_uint64());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(50 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(150 * UNIT), supply());
} FC_LOG_AND_RETHROW()

/// Outstanding returns retain the original beneficiary and exact obligation, without lifetime totals.
BOOST_FIXTURE_TEST_CASE(outstanding_returns_record_the_original_obligation, sysio_synd_tester) try {
   BOOST_REQUIRE_EQUAL(success(), liq_mint("alice"_n, LIQSOL, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), liq_mint("alice"_n, LIQTWO, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, ed_key()));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {}));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQTWO, {}));

   BOOST_REQUIRE_EQUAL(success(), desyndicate("alice"_n, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), desyndicate("alice"_n, 5 * UNIT, LIQTWO_SYM));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("overdrawn balance"), desyndicate("alice"_n, 1'000 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), desyndicate("alice"_n, 7 * UNIT));

   const auto check_log = [&](uint64_t id, std::string_view token, uint64_t amount) {
      const auto row = return_row(id);
      BOOST_TEST_CONTEXT("request " << id) {
         BOOST_REQUIRE(!row.is_null());
         BOOST_REQUIRE_EQUAL(id, row["request_id"].as_uint64());
         BOOST_REQUIRE_EQUAL(SOLANA, row["chain_code"].as_string());
         BOOST_REQUIRE_EQUAL(token, row["token_code"].as_string());
         BOOST_REQUIRE_EQUAL(amount, row["amount"].as_uint64());
         BOOST_REQUIRE_EQUAL("alice", row["holder"].as_string());
         BOOST_REQUIRE_EQUAL(32u, row["pubkey"].as<std::vector<char>>().size());
      }
   };
   check_log(1, LIQSOL, 10 * UNIT);
   check_log(2, LIQTWO, 5 * UNIT);
   check_log(3, LIQSOL, 7 * UNIT);
   BOOST_REQUIRE(return_row(4).is_null());

   BOOST_REQUIRE(ledger_row(SOLANA, LIQSOL).is_null());

   // Only intake needs operational queue positions.
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest_of("envelope-1"), 1, ChainKind::CHAIN_KIND_SVM,
                                         ed_bytes(ed_key()), LIQSOL, 3 * UNIT));
} FC_LOG_AND_RETHROW()

/// Governance closes externally settled returns or refunds a definitively cancelled obligation.
/// Both consume the pending record exactly once; fees stay charged and refund destinations are immutable.
BOOST_FIXTURE_TEST_CASE(return_resolution_is_authorized_exact_and_once, sysio_synd_tester) try {
   BOOST_REQUIRE_EQUAL(success(), liq_mint("alice"_n, LIQSOL, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, ed_key()));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.desynd_fee_bps = 1000}));
   BOOST_REQUIRE_EQUAL(success(), desyndicate("alice"_n, 10 * UNIT));
   const auto resolve = [&](name signer, name action, uint64_t id) {
      return push(SYND_ACCOUNT, synd_abi_ser, signer, action, mvo()("request_id", id));
   };
   BOOST_REQUIRE(mentions(resolve("alice"_n, "refundreturn"_n, 1), "missing authority of sysio"));
   BOOST_REQUIRE(mentions(resolve("alice"_n, "finishreturn"_n, 1), "missing authority of sysio"));
   BOOST_REQUIRE(!return_row(1).is_null());
   BOOST_REQUIRE_EQUAL(success(), resolve(SYSIO_ACCOUNT, "refundreturn"_n, 1));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(99 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(UNIT, feepot_balance(LIQSOL));
   BOOST_REQUIRE(return_row(1).is_null());
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("outstanding return not found"), resolve(SYSIO_ACCOUNT, "refundreturn"_n, 1));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("outstanding return not found"), resolve(SYSIO_ACCOUNT, "finishreturn"_n, 1));
   BOOST_REQUIRE_EQUAL(success(), desyndicate("alice"_n, 10 * UNIT));
   const auto before = liq_balance("alice"_n);
   BOOST_REQUIRE_EQUAL(success(), resolve(SYSIO_ACCOUNT, "finishreturn"_n, 2));
   BOOST_REQUIRE_EQUAL(before, liq_balance("alice"_n));
   BOOST_REQUIRE(return_row(2).is_null());
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("outstanding return not found"), resolve(SYSIO_ACCOUNT, "refundreturn"_n, 2));
   BOOST_REQUIRE_EQUAL(3u, counters_row()["next_request_id"].as_uint64());
} FC_LOG_AND_RETHROW()

/// A failed inline restoration leaves the outstanding record available for later recovery.
BOOST_FIXTURE_TEST_CASE(return_refund_failure_preserves_obligation, sysio_synd_tester) try {
   BOOST_REQUIRE_EQUAL(success(), liq_mint("alice"_n, LIQSOL, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, ed_key()));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {}));
   BOOST_REQUIRE_EQUAL(success(), desyndicate("alice"_n, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), liq_mint("bob"_n, LIQSOL, static_cast<uint64_t>(asset::max_amount)));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("supply exceeds the asset range"),
      push(SYND_ACCOUNT, synd_abi_ser, SYSIO_ACCOUNT, "refundreturn"_n, mvo()("request_id", 1)));
   BOOST_REQUIRE(!return_row(1).is_null());
   BOOST_REQUIRE_EQUAL(0, liq_balance("alice"_n));
} FC_LOG_AND_RETHROW()

/// The DESYNDICATE_LIQ attestation `id` queued, decoded.
sysio::opp::attestations::DesyndicateLIQ queued_desyndication(sysio_synd_tester& t, uint64_t id) {
   const auto att = t.attestation_row(id);
   BOOST_REQUIRE(!att.is_null());
   BOOST_REQUIRE_EQUAL("ATTESTATION_TYPE_DESYNDICATE_LIQ", att["type"].as_string());
   const auto data = att["data"].as<std::vector<char>>();
   sysio::opp::attestations::DesyndicateLIQ msg;
   BOOST_REQUIRE(msg.ParseFromArray(data.data(), static_cast<int>(data.size())));
   return msg;
}

// Review Focus 6: a DESYNDICATE_LIQ carries the depot's outstanding shadow after its own burn -- the
// supply plus the yield parked in liqpending, net of the burned amount -- and `desyndlog` records the
// same total. The fee stays outstanding: it is transferred into the fee pot, not burned. The carried
// total and the headroom add up to the asset range: after the burn the ledger admits a mint of exactly
// the range net of that total, and not one base unit more.
BOOST_FIXTURE_TEST_CASE(desyndicate_carries_the_outstanding_shadow_after_its_burn, sysio_synd_tester) try {
   BOOST_REQUIRE_EQUAL(success(), liq_mint("alice"_n, LIQSOL, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), liq_mintyield(SOLANA, LIQSOL, 7 * UNIT));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(7 * UNIT), pending());
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, ed_key()));
   BOOST_REQUIRE_EQUAL(success(),
                       setconfig(SOLANA, LIQSOL, {.desynd_fee_bps = 250, .desynd_burst = 100 * UNIT}));

   // 40 leave alice: a fee of 1 stays in the pot and 39 burn, leaving 61 supply and 7 pending.
   BOOST_REQUIRE_EQUAL(success(), desyndicate("alice"_n, 40 * UNIT));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(61 * UNIT), supply());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(7 * UNIT), pending());
   const uint64_t first_total = 68 * UNIT;
   BOOST_REQUIRE_EQUAL(first_total, queued_desyndication(*this, 1).total_syndicated());

   // The next one carries the total after its own burn: 20 with a fee of 0.5 burns 19.5.
   BOOST_REQUIRE_EQUAL(success(), desyndicate("alice"_n, 20 * UNIT));
   const uint64_t second_total = first_total - (19 * UNIT + UNIT / 2);
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(second_total - 7 * UNIT), supply());
   BOOST_REQUIRE_EQUAL(second_total, queued_desyndication(*this, 2).total_syndicated());

   // Outstanding plus headroom is the asset range.
   const uint64_t range = static_cast<uint64_t>(asset::max_amount);
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("supply exceeds the asset range"),
                       liq_mint("alice"_n, LIQSOL, range - second_total + 1));
   BOOST_REQUIRE_EQUAL(success(), liq_mint("alice"_n, LIQSOL, range - second_total));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(range - 7 * UNIT), supply());
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("supply exceeds the asset range"), liq_mint("alice"_n, LIQSOL, 1));
} FC_LOG_AND_RETHROW()

// The fee, `quantity * desynd_fee_bps / 10000` floored, stays in `feepot` in sysio.synd's own row; only
// the net rest is burned, queued, logged and summed, and the bucket drops by the whole quantity. A fee of
// the whole quantity leaves nothing to release on the outpost and is refused.
BOOST_FIXTURE_TEST_CASE(desyndicate_charges_the_fee_and_queues_the_net_amount, sysio_synd_tester) try {
   BOOST_REQUIRE_EQUAL(success(), liq_mint("alice"_n, LIQSOL, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, ed_key()));
   BOOST_REQUIRE_EQUAL(success(),
                       setconfig(SOLANA, LIQSOL, {.desynd_fee_bps = 250, .desynd_burst = 100 * UNIT}));

   BOOST_REQUIRE_EQUAL(success(), desyndicate("alice"_n, 40 * UNIT));
   // 2.5% of 40 is 1: the holder pays 40, the fee pot keeps 1, 39 are burned and queued to the outpost.
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(60 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(UNIT, feepot_balance(LIQSOL));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(UNIT), liq_balance(SYND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(61 * UNIT), supply());

   const auto msg = queued_desyndication(*this, 1);
   BOOST_REQUIRE_EQUAL(slug_value(LIQSOL), msg.amount().token_code());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(39 * UNIT), msg.amount().amount());
   BOOST_REQUIRE_EQUAL(1u, msg.request_id());
   const auto log = return_row(1);
   BOOST_REQUIRE_EQUAL(39 * UNIT, log["amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(60 * UNIT, desynd_bucket_row(SOLANA, LIQSOL)["level"].as_uint64());
   // The syndication direction is untouched.
   BOOST_REQUIRE(synd_bucket_row(SOLANA, LIQSOL).is_null());

   // A second desyndication adds its fee to the same pot.
   BOOST_REQUIRE_EQUAL(success(), desyndicate("alice"_n, 20 * UNIT));
   BOOST_REQUIRE_EQUAL(UNIT + UNIT / 2, feepot_balance(LIQSOL));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(UNIT + UNIT / 2), liq_balance(SYND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(19 * UNIT + UNIT / 2), queued_desyndication(*this, 2).amount().amount());
   BOOST_REQUIRE_EQUAL(40 * UNIT, desynd_bucket_row(SOLANA, LIQSOL)["level"].as_uint64());

   // The whole quantity as a fee is refused, and changes nothing.
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.desynd_fee_bps = 10000, .desynd_burst = 100 * UNIT}));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("desyndication net of the fee must be positive"),
                       desyndicate("alice"_n, UNIT));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(40 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(3u, counters_row()["next_request_id"].as_uint64());
} FC_LOG_AND_RETHROW()

// A pair with no `syndconfig` row has no desyndication budget; a quantity above the bucket's level is
// refused, however much the holder holds, and a refusal changes nothing.
BOOST_FIXTURE_TEST_CASE(desyndicate_refuses_over_budget, sysio_synd_tester) try {
   BOOST_REQUIRE_EQUAL(success(), liq_mint("alice"_n, LIQSOL, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, ed_key()));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("desyndication budget is not configured"), desyndicate("alice"_n, UNIT));

   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.desynd_burst = 10 * UNIT, .desynd_refill = 4 * UNIT}));
   BOOST_REQUIRE_EQUAL(success(), set_epoch(5));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("desyndication exceeds the current budget"),
                       desyndicate("alice"_n, 10 * UNIT + 1));
   BOOST_REQUIRE(desynd_bucket_row(SOLANA, LIQSOL).is_null());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(100 * UNIT), liq_balance("alice"_n));

   // A new bucket starts full: the whole burst passes, then nothing until it refills.
   BOOST_REQUIRE_EQUAL(success(), desyndicate("alice"_n, 10 * UNIT));
   const auto bucket = desynd_bucket_row(SOLANA, LIQSOL);
   BOOST_REQUIRE_EQUAL(0u, bucket["level"].as_uint64());
   BOOST_REQUIRE_EQUAL(5u, bucket["last_epoch"].as<uint32_t>());
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("desyndication exceeds the current budget"), desyndicate("alice"_n, 1));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(90 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(2u, counters_row()["next_request_id"].as_uint64());
   BOOST_REQUIRE(return_row(2).is_null());
} FC_LOG_AND_RETHROW()

// The bucket refills by `desynd_refill` per depot epoch up to the burst, and drops by the whole quantity
// a desyndication moves, its fee included.
BOOST_FIXTURE_TEST_CASE(passes_after_refill, sysio_synd_tester) try {
   BOOST_REQUIRE_EQUAL(success(), liq_mint("alice"_n, LIQSOL, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, ed_key()));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.desynd_fee_bps = 1000, .desynd_burst = 10 * UNIT,
                                                             .desynd_refill = 4 * UNIT}));
   BOOST_REQUIRE_EQUAL(success(), set_epoch(5));
   BOOST_REQUIRE_EQUAL(success(), desyndicate("alice"_n, 10 * UNIT));
   // 9 queued, 1 kept as the fee, and the bucket emptied by all 10.
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(9 * UNIT), queued_desyndication(*this, 1).amount().amount());
   BOOST_REQUIRE_EQUAL(0u, desynd_bucket_row(SOLANA, LIQSOL)["level"].as_uint64());

   // One epoch later 4 have refilled.
   BOOST_REQUIRE_EQUAL(success(), set_epoch(6));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("desyndication exceeds the current budget"),
                       desyndicate("alice"_n, 4 * UNIT + 1));
   BOOST_REQUIRE_EQUAL(success(), desyndicate("alice"_n, 4 * UNIT));
   BOOST_REQUIRE_EQUAL(0u, desynd_bucket_row(SOLANA, LIQSOL)["level"].as_uint64());
   BOOST_REQUIRE_EQUAL(6u, desynd_bucket_row(SOLANA, LIQSOL)["last_epoch"].as<uint32_t>());

   // Three epochs refill 12, capped at the burst of 10.
   BOOST_REQUIRE_EQUAL(success(), set_epoch(9));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("desyndication exceeds the current budget"),
                       desyndicate("alice"_n, 10 * UNIT + 1));
   BOOST_REQUIRE_EQUAL(success(), desyndicate("alice"_n, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(76 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(24 * UNIT) / 10, liq_balance(SYND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(24 * UNIT / 10, feepot_balance(LIQSOL));
} FC_LOG_AND_RETHROW()

// Review Focus 5: a fee that rounds down to zero keeps nothing; the whole quantity is burned, queued,
// logged and summed, and no fee pot row appears.
BOOST_FIXTURE_TEST_CASE(desyndicate_fee_rounding_to_zero, sysio_synd_tester) try {
   // 333 base units at 30 bps is 0.999 of a unit: the fee floors to 0.
   constexpr uint64_t quantity = 333;
   BOOST_REQUIRE_EQUAL(success(), liq_mint("alice"_n, LIQSOL, UNIT));
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, ed_key()));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.desynd_fee_bps = 30}));
   BOOST_REQUIRE_EQUAL(success(), desyndicate("alice"_n, quantity));

   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(UNIT - quantity), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(UNIT - quantity), supply());
   BOOST_REQUIRE_EQUAL(0, liq_balance(SYND_ACCOUNT));
   BOOST_REQUIRE(feepot_row(LIQSOL).is_null());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(quantity), queued_desyndication(*this, 1).amount().amount());
   BOOST_REQUIRE_EQUAL(quantity, return_row(1)["amount"].as_uint64());

   // One more base unit makes the fee 1: 334 * 30 / 10000 is 1.002, floored to 1.
   BOOST_REQUIRE_EQUAL(success(), desyndicate("alice"_n, quantity + 1));
   BOOST_REQUIRE_EQUAL(1u, feepot_balance(LIQSOL));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(quantity), queued_desyndication(*this, 2).amount().amount());
} FC_LOG_AND_RETHROW()

// Only a balance in the holder's own sysio.liq row can be desyndicated: a syndication held for the
// holder's linked pubkey and a balance parked for another pubkey sit in sysio.synd's row and cannot be
// drawn, and a refusal leaves both where they were.
BOOST_FIXTURE_TEST_CASE(desyndicate_refuses_held_or_parked_funds, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   const auto parked_pubkey = ed_bytes(ed_key());
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {}));
   BOOST_REQUIRE_EQUAL(success(), importsynd(SOLANA, LIQSOL, { credit(parked_pubkey, 20 * UNIT) }));
   syndicate_and_close(1, 1, pubkey, LIQSOL, 50 * UNIT);
   BOOST_REQUIRE_EQUAL(50 * UNIT, item_row(1)["remaining"].as_uint64());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(70 * UNIT), liq_balance(SYND_ACCOUNT));

   // Alice has no row of her own: nothing to desyndicate.
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("no balance object found"), desyndicate("alice"_n, UNIT));
   // With 5 of her own, 6 is overdrawn.
   BOOST_REQUIRE_EQUAL(success(), liq_mint("alice"_n, LIQSOL, 5 * UNIT));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("overdrawn balance"), desyndicate("alice"_n, 6 * UNIT));
   BOOST_REQUIRE_EQUAL(50 * UNIT, item_row(1)["remaining"].as_uint64());
   BOOST_REQUIRE_EQUAL(20 * UNIT, parked_balance(LIQSOL, ChainKind::CHAIN_KIND_SVM, parked_pubkey));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(70 * UNIT), liq_balance(SYND_ACCOUNT));

   // Her own 5 pass; the held and parked balances do not move.
   BOOST_REQUIRE_EQUAL(success(), desyndicate("alice"_n, 5 * UNIT));
   BOOST_REQUIRE_EQUAL(0, liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(50 * UNIT, item_row(1)["remaining"].as_uint64());
   BOOST_REQUIRE_EQUAL(20 * UNIT, parked_balance(LIQSOL, ChainKind::CHAIN_KIND_SVM, parked_pubkey));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(70 * UNIT), liq_balance(SYND_ACCOUNT));
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Launch ingestion
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(importsynd_parks_or_credits_and_importdone_closes, sysio_synd_tester) try {
   const auto key_a = ed_key(), key_b = ed_key(), key_c = ed_key();
   const auto a = ed_bytes(key_a), b = ed_bytes(key_b), c = ed_bytes(key_c);
   BOOST_REQUIRE_EQUAL(success(), link_svm("carol"_n, key_c));

   BOOST_REQUIRE_EQUAL(success(), importsynd(SOLANA, LIQSOL, { credit(a, 30 * UNIT), credit(b, 70 * UNIT),
                                                               credit(c, 5 * UNIT) }));
   BOOST_REQUIRE_EQUAL(30 * UNIT, parked_balance(LIQSOL, ChainKind::CHAIN_KIND_SVM, a));
   BOOST_REQUIRE_EQUAL(70 * UNIT, parked_balance(LIQSOL, ChainKind::CHAIN_KIND_SVM, b));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(100 * UNIT), liq_balance(SYND_ACCOUNT));   // what is parked
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(5 * UNIT), liq_balance("carol"_n));   // already linked: straight in
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(105 * UNIT), supply());

   // The same pubkey across batches sums; zero credits are skipped.
   BOOST_REQUIRE_EQUAL(success(), importsynd(SOLANA, LIQSOL, { credit(a, 10 * UNIT), credit(b, 0) }));
   BOOST_REQUIRE_EQUAL(40 * UNIT, parked_balance(LIQSOL, ChainKind::CHAIN_KIND_SVM, a));
   BOOST_REQUIRE_EQUAL(70 * UNIT, parked_balance(LIQSOL, ChainKind::CHAIN_KIND_SVM, b));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(115 * UNIT), supply());

   // Every refusal aborts the whole batch.
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("pubkey does not fit the chain family"),
                       importsynd(SOLANA, LIQSOL, { credit(std::vector<char>(33, 'x'), UNIT) }));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("token_code belongs to another chain"),
                       importsynd(ETH, LIQSOL, { credit(a, UNIT) }));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("token_code has no shadow symbol"),
                       importsynd(SOLANA, LIQNONE, { credit(a, UNIT) }));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("amount out of range"),
                       importsynd(SOLANA, LIQSOL, { credit(a, DEPOT_AMOUNT_MAX + 1) }));
   BOOST_REQUIRE(mentions(importsynd(SOLANA, LIQSOL, { credit(a, UNIT) }, "alice"_n),
                          "missing authority of sysio.synd"));
   BOOST_REQUIRE(mentions(importdone("alice"_n), "missing authority of sysio.synd"));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(115 * UNIT), supply());

   BOOST_REQUIRE_EQUAL(success(), importdone());
   BOOST_REQUIRE(state_row()["import_complete"].as_bool());
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("import already finalized"), importsynd(SOLANA, LIQSOL, { credit(a, UNIT) }));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("import already finalized"), importdone());

   // The imported positions earn from the first distribution on.
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 23 * UNIT));   // 115 supply: one WIRE per five shadow
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key_a));
   const int64_t alice_wire = wire_balance("alice"_n);
   BOOST_REQUIRE_EQUAL(success(), sweep("alice"_n, ChainKind::CHAIN_KIND_SVM));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(40 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(8 * UNIT), liq_owed("alice"_n));
   BOOST_REQUIRE_EQUAL(success(), liq_claim("alice"_n));
   BOOST_REQUIRE_EQUAL(alice_wire + static_cast<int64_t>(8 * UNIT), wire_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(UNIT), liq_owed("carol"_n));
   // b's row still waits in sysio.synd's row, owed the rest of what the pool pulled.
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(70 * UNIT), liq_balance(SYND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(22 * UNIT, pool_row(LIQSOL)["pool"]["received"].as_uint64());
   BOOST_REQUIRE_EQUAL(8 * UNIT, pool_row(LIQSOL)["pool"]["credited"].as_uint64());
} FC_LOG_AND_RETHROW()

// The bootstrap tool replays the config row by row: the chains, the LIQ tokens with their bindings,
// one `create` per shadow, one `regliqpool` per pool, `importsynd` per syndication, then `importdone`.
// Replaying the checked-in dev config proves the values the validator accepts are values the
// contracts accept, and pins the accounting: each pool holds exactly its two seeds, the shadow supply
// is the LCO seed plus the parked syndications, all of which sit in sysio.synd's own row, every index
// starts at 0, the WIRE drained is the sum of the pools' WIRE seeds within the dex earmark, and the
// import closes for good.
BOOST_FIXTURE_TEST_CASE(the_dev_config_replays_into_the_shadow_liq_system, sysio_synd_config_tester) try {
   const auto cfg = load_dev_config();
   BOOST_REQUIRE(!cfg.liq_pools().empty());
   BOOST_REQUIRE(!cfg.syndications().empty());

   std::map<std::string, ChainKind> kind_of_chain;
   for (const auto& chain : cfg.chains()) {
      kind_of_chain[chain.code()] = chain.kind();
      BOOST_REQUIRE_EQUAL(success(), regchain(chain.kind(), chain.code(), chain.external_chain_id()));
   }
   std::map<std::string, symbol> shadow_of_token;
   for (const auto& token : cfg.tokens()) {
      if (token.kind() != TokenKind::TOKEN_KIND_LIQ) continue;   // the shadow ledger binds only liq tokens
      const auto chain_kind = kind_of_chain.at(token.chain_code());
      shadow_of_token.emplace(token.code(), symbol(static_cast<uint8_t>(token.precision()), token.code()));
      BOOST_REQUIRE_EQUAL(success(), regtoken(token.code(), token.precision(), chain_kind, token.chain_code(),
                                              address_bytes(chain_kind, token.contract_address())));
   }

   const int64_t treasury_before = wire_balance(SYSIO_ACCOUNT);
   uint64_t      wire_drained    = 0;
   for (const auto& pool : cfg.liq_pools()) {
      const symbol shadow = shadow_of_token.at(pool.token_code());
      const symbol pair(9, pool.pair_symbol());
      BOOST_REQUIRE_EQUAL(success(), create_shadow(shadow, pool.chain_code(), pool.token_code()));
      BOOST_REQUIRE_EQUAL(success(), create_pool(pool.chain_code(), pool.token_code(), pair,
                                                 pool.initial_chain_amount(), pool.initial_wire_amount(),
                                                 static_cast<int32_t>(pool.fee()),
                                                 static_cast<int64_t>(pool.locked_shares()),
                                                 pool.conversion_horizon_sec(), pool.depth_cap_bps(),
                                                 static_cast<int64_t>(pool.clip_floor())));
      wire_drained += pool.initial_wire_amount();

      const auto pool_stat = swap_pool_row(pair);
      BOOST_REQUIRE_MESSAGE(!pool_stat.is_null(), "no swap pair " << pool.pair_symbol());
      BOOST_REQUIRE_EQUAL(static_cast<int64_t>(pool.initial_chain_amount()),
                          pool_stat["pool1"]["quantity"].as<asset>().get_amount());
      BOOST_REQUIRE_EQUAL(static_cast<int64_t>(pool.initial_wire_amount()),
                          pool_stat["pool2"]["quantity"].as<asset>().get_amount());
      // The index row is materialized by the first distribution; until then it reads as 0, which is
      // what every seeded holder and parked position is stamped at.
      BOOST_REQUIRE_EQUAL(fc::uint128_t{0}, liq_index(shadow));
   }

   // Nobody is AuthX-linked at bootstrap, so every syndication parks against its pubkey; repeated
   // pubkeys sum.
   std::map<std::pair<std::string, std::string>, uint64_t> parked_of;   // (token_code, pubkey) -> amount
   for (const auto& synd : cfg.syndications()) {
      const auto chain_kind = kind_of_chain.at(synd.chain_code());
      BOOST_REQUIRE_EQUAL(success(), importsynd(synd.chain_code(), synd.token_code(),
                                                { credit(address_bytes(chain_kind, synd.pubkey()), synd.amount()) }));
      parked_of[{ synd.token_code(), synd.pubkey() }] += synd.amount();
   }
   BOOST_REQUIRE_EQUAL(success(), importdone());
   {
      const auto& first = cfg.syndications(0);
      BOOST_REQUIRE_EQUAL(wasm_assert_msg("import already finalized"),
                          importsynd(first.chain_code(), first.token_code(),
                                     { credit(address_bytes(kind_of_chain.at(first.chain_code()), first.pubkey()),
                                              first.amount()) }));
   }

   for (const auto& pool : cfg.liq_pools()) {
      const symbol shadow     = shadow_of_token.at(pool.token_code());
      const auto   chain_kind = kind_of_chain.at(pool.chain_code());
      uint64_t     syndicated = 0;
      for (const auto& [key, amount] : parked_of) {
         if (key.first != pool.token_code()) continue;
         syndicated += amount;
         BOOST_REQUIRE_MESSAGE(parked_balance(pool.token_code(), chain_kind, address_bytes(chain_kind, key.second)) ==
                                  amount,
                               "parked row of " << key.second);
      }
      BOOST_REQUIRE_EQUAL(static_cast<int64_t>(pool.initial_chain_amount() + syndicated), supply(shadow));
      BOOST_REQUIRE_EQUAL(static_cast<int64_t>(syndicated), liq_balance(SYND_ACCOUNT, shadow));
      // The config's custody cross-check is the contract's truth: the outpost custodies exactly what the
      // depot minted against.
      if (pool.custody_total() != 0)
         BOOST_REQUIRE_EQUAL(pool.custody_total(), pool.initial_chain_amount() + syndicated);
   }

   BOOST_REQUIRE_EQUAL(treasury_before - static_cast<int64_t>(wire_drained), wire_balance(SYSIO_ACCOUNT));
   BOOST_REQUIRE_LE(wire_drained, cfg.t5_dex_allocation());
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// The queue: underwriting requests
// ---------------------------------------------------------------------------

// Review Focus 2: an envelope that carried two tokens is closed into one sysio.bond request per token,
// each for its own statement and covering the token's syndicated plus yield total rounded UP to the bond
// increment. Bonding one of them releases it while the other still waits, and neither blocks the other.
BOOST_FIXTURE_TEST_CASE(closeenv_issues_one_request_per_token, sysio_synd_tester) try {
   const auto     key    = ed_key();
   const auto     pubkey = ed_bytes(key);
   const auto     digest = digest_of("envelope-1");
   constexpr auto SVM    = ChainKind::CHAIN_KIND_SVM;
   constexpr uint32_t LIQTWO_WINDOW_SEC = 3600;
   constexpr uint64_t INCREMENT         = 10'000'000;   // 0.01 token at precision 9
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {}));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQTWO, {.window_sec = LIQTWO_WINDOW_SEC}));
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 1, SVM, pubkey, LIQSOL, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), onyield(SOLANA, 1, digest, 2, 7, LIQSOL, 2 * UNIT));
   // Three base units past a whole token: covered rounds up to the next increment.
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 3, SVM, pubkey, LIQTWO, 5 * UNIT + 3));
   BOOST_REQUIRE_EQUAL(1u, next_bond_request_id());
   BOOST_REQUIRE_EQUAL(success(), closeenv(SOLANA, 1, digest));
   BOOST_REQUIRE_EQUAL(3u, next_bond_request_id());

   const auto liqsol = envelope_row(SOLANA, LIQSOL, 1);
   const auto liqtwo = envelope_row(SOLANA, LIQTWO, 1);
   BOOST_REQUIRE_EQUAL(STATE_REQUESTED, liqsol["state"].as_string());
   BOOST_REQUIRE_EQUAL(STATE_REQUESTED, liqtwo["state"].as_string());
   const uint64_t sol_id = liqsol["request_id"].as_uint64();
   const uint64_t two_id = liqtwo["request_id"].as_uint64();
   BOOST_REQUIRE_NE(sol_id, two_id);

   const auto sol_request = bond_request(sol_id);
   BOOST_REQUIRE_EQUAL(SYND_ACCOUNT.to_string(), sol_request["issuer"].as_string());
   BOOST_REQUIRE_EQUAL(STATEMENT_SCHEMA, sol_request["schema"].as_string());
   BOOST_REQUIRE(statement_of(SOLANA, 1, digest, LIQSOL) == sol_request["statement"].as<std::vector<char>>());
   BOOST_REQUIRE_EQUAL(LIQSOL, sol_request["token_code"].as_string());
   BOOST_REQUIRE_EQUAL(12 * UNIT, sol_request["covered"].as_uint64());
   BOOST_REQUIRE_EQUAL(0u, sol_request["bounty"].as_uint64());
   BOOST_REQUIRE_EQUAL(DEFAULT_WINDOW_SEC, sol_request["window_sec"].as<uint32_t>());
   BOOST_REQUIRE_EQUAL(REQUEST_OPEN, sol_request["state"].as_string());
   const auto two_request = bond_request(two_id);
   BOOST_REQUIRE(statement_of(SOLANA, 1, digest, LIQTWO) == two_request["statement"].as<std::vector<char>>());
   BOOST_REQUIRE_EQUAL(LIQTWO, two_request["token_code"].as_string());
   BOOST_REQUIRE_EQUAL(5 * UNIT + INCREMENT, two_request["covered"].as_uint64());
   BOOST_REQUIRE_EQUAL(LIQTWO_WINDOW_SEC, two_request["window_sec"].as<uint32_t>());

   // LIQTWO is bonded and LIQSOL is not: LIQTWO releases, LIQSOL keeps waiting for its own request.
   underwrite_envelope(SOLANA, LIQTWO, 1);
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(STATE_DONE, envelope_row(SOLANA, LIQTWO, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(5 * UNIT + 3), liq_balance("alice"_n, LIQTWO_SYM));
   BOOST_REQUIRE_EQUAL(STATE_REQUESTED, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(0, liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(0, pending());

   // Bonding LIQSOL releases it too: the syndication to the account, the yield into pending yield.
   underwrite_envelope(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(STATE_DONE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(10 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(2 * UNIT), pending());
   BOOST_REQUIRE_EQUAL(3u, next_bond_request_id());
} FC_LOG_AND_RETHROW()

// One request at a time per outpost and token, in envelope order: a later envelope stays WAITING while
// an earlier request is neither bonded nor ruled -- a request held before it was bonded included -- and
// is requested once the earlier one is bonded or ruled. A hold placed after bonding does not stop the
// queue.
BOOST_FIXTURE_TEST_CASE(second_envelope_waits_until_the_first_is_bonded, sysio_synd_tester) try {
   const auto pubkey = ed_bytes(ed_key());
   syndicate_and_close(1, 1, pubkey, LIQSOL, 10 * UNIT);
   syndicate_and_close(2, 2, pubkey, LIQSOL, 4 * UNIT);
   syndicate_and_close(3, 3, pubkey, LIQSOL, 6 * UNIT);
   BOOST_REQUIRE_EQUAL(STATE_REQUESTED, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(1u, envelope_row(SOLANA, LIQSOL, 1)["request_id"].as_uint64());
   BOOST_REQUIRE_EQUAL(STATE_WAITING, envelope_row(SOLANA, LIQSOL, 2)["state"].as_string());
   BOOST_REQUIRE_EQUAL(0u, envelope_row(SOLANA, LIQSOL, 2)["request_id"].as_uint64());
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(STATE_WAITING, envelope_row(SOLANA, LIQSOL, 2)["state"].as_string());
   BOOST_REQUIRE_EQUAL(2u, next_bond_request_id());

   // Held before it was bonded: the queue waits for sysio's ruling.
   BOOST_REQUIRE_EQUAL(success(), push(BOND_ACCOUNT, bond_abi_ser, SYND_ACCOUNT, "hold"_n, mvo()
      ("request_id", 1)("beneficiary", "carol"_n)));
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(STATE_HELD, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(STATE_WAITING, envelope_row(SOLANA, LIQSOL, 2)["state"].as_string());
   BOOST_REQUIRE_EQUAL(2u, next_bond_request_id());

   // Ruled VALID: the first becomes RELEASABLE and the second is requested.
   BOOST_REQUIRE_EQUAL(success(), push(BOND_ACCOUNT, bond_abi_ser, SYSIO_ACCOUNT, "rslvvalid"_n, mvo()
      ("request_id", 1)));
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(STATE_RELEASABLE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(STATE_REQUESTED, envelope_row(SOLANA, LIQSOL, 2)["state"].as_string());
   BOOST_REQUIRE_EQUAL(2u, envelope_row(SOLANA, LIQSOL, 2)["request_id"].as_uint64());
   BOOST_REQUIRE(statement_of(SOLANA, 2, digest_of("envelope-2"), LIQSOL) ==
                 bond_request(2)["statement"].as<std::vector<char>>());
   BOOST_REQUIRE_EQUAL(STATE_WAITING, envelope_row(SOLANA, LIQSOL, 3)["state"].as_string());

   // Bonded, then held: the hold stops the second envelope's release but not the queue.
   underwrite_envelope(SOLANA, LIQSOL, 2);
   BOOST_REQUIRE_EQUAL(REQUEST_BONDED, bond_request(2)["state"].as_string());
   BOOST_REQUIRE_EQUAL(success(), push(BOND_ACCOUNT, bond_abi_ser, SYND_ACCOUNT, "hold"_n, mvo()
      ("request_id", 2)("beneficiary", "carol"_n)));
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(STATE_HELD, envelope_row(SOLANA, LIQSOL, 2)["state"].as_string());
   BOOST_REQUIRE_EQUAL(STATE_REQUESTED, envelope_row(SOLANA, LIQSOL, 3)["state"].as_string());
   BOOST_REQUIRE_EQUAL(3u, envelope_row(SOLANA, LIQSOL, 3)["request_id"].as_uint64());
} FC_LOG_AND_RETHROW()

// A request that is only partly bonded keeps its envelope REQUESTED. Fully bonded, the envelope is
// RELEASABLE; a VALID ruling keeps it so.
BOOST_FIXTURE_TEST_CASE(bonded_request_makes_the_envelope_releasable, sysio_synd_tester) try {
   const auto pubkey = ed_bytes(ed_key());
   syndicate_and_close(1, 1, pubkey, LIQSOL, 10 * UNIT);
   const uint64_t id = envelope_row(SOLANA, LIQSOL, 1)["request_id"].as_uint64();

   BOOST_REQUIRE_EQUAL(success(), underwrite("dave"_n, id, 4 * UNIT, LIQSOL));
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(REQUEST_OPEN, bond_request(id)["state"].as_string());
   BOOST_REQUIRE_EQUAL(STATE_REQUESTED, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());

   BOOST_REQUIRE_EQUAL(success(), underwrite("bob"_n, id, 6 * UNIT, LIQSOL));
   BOOST_REQUIRE_EQUAL(REQUEST_BONDED, bond_request(id)["state"].as_string());
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(STATE_RELEASABLE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());

   BOOST_REQUIRE_EQUAL(success(), push(BOND_ACCOUNT, bond_abi_ser, SYSIO_ACCOUNT, "rslvvalid"_n, mvo()
      ("request_id", id)));
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(STATE_RELEASABLE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
} FC_LOG_AND_RETHROW()

// The bounty posted on a request is the configured one, or what the fee pot holds when that is less:
// 0 while the pot is empty. sysio.bond pulls it out of sysio.synd's row, where the pot kept it.
BOOST_FIXTURE_TEST_CASE(bounty_comes_from_the_fee_pot_or_is_zero, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.synd_fee_bps = 1000, .bounty = UNIT}));
   syndicate_and_close(1, 1, pubkey, LIQSOL, 20 * UNIT);
   syndicate_and_close(2, 2, pubkey, LIQSOL, 10 * UNIT);
   BOOST_REQUIRE_EQUAL(0u, bond_request(1)["bounty"].as_uint64());

   // The first release pays 2 into the pot; the second request takes its configured 1 out of it.
   underwrite_envelope(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(18 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(STATE_REQUESTED, envelope_row(SOLANA, LIQSOL, 2)["state"].as_string());
   BOOST_REQUIRE_EQUAL(UNIT, bond_request(2)["bounty"].as_uint64());
   BOOST_REQUIRE_EQUAL(UNIT, feepot_balance(LIQSOL));
   // What sysio.synd still holds: the second envelope's 10 and the pot's 1.
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(11 * UNIT), liq_balance(SYND_ACCOUNT));

   // Configured above what the pot holds: the bounty is the whole pot.
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.synd_fee_bps = 1000, .bounty = 5 * UNIT}));
   syndicate_and_close(3, 3, pubkey, LIQSOL, 4 * UNIT);
   underwrite_envelope(SOLANA, LIQSOL, 2);
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(27 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(2 * UNIT, bond_request(3)["bounty"].as_uint64());
   BOOST_REQUIRE_EQUAL(0u, feepot_balance(LIQSOL));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(4 * UNIT), liq_balance(SYND_ACCOUNT));
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// The queue: release
// ---------------------------------------------------------------------------

// A bonded syndication pays its fee into the fee pot and the rest to the linked account, and credits
// the WIRE the item earned while it was held to the account's own sysio.liq row; the item is erased,
// the envelope is DONE and the pair's queue cursor moves past it.
BOOST_FIXTURE_TEST_CASE(release_pays_the_fee_and_credits_the_account, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.synd_fee_bps = 250}));
   BOOST_REQUIRE_EQUAL(success(), liq_mint("bob"_n, LIQSOL, 40 * UNIT));
   syndicate_and_close(1, 1, pubkey, LIQSOL, 40 * UNIT);
   // Yield lands while the item is held: half the supply is the item's.
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 10 * UNIT));
   const int64_t owed = item_owed(1);
   BOOST_REQUIRE_GT(owed, 0);
   const int64_t alice_wire = wire_balance("alice"_n);

   underwrite_envelope(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE(item_row(1).is_null());
   const auto envelope = envelope_row(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(STATE_DONE, envelope["state"].as_string());
   BOOST_REQUIRE_EQUAL(40 * UNIT, envelope["released"].as_uint64());
   // 2.5% of 40 is 1.
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(39 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(UNIT, feepot_balance(LIQSOL));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(UNIT), liq_balance(SYND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(alice_wire, wire_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(owed, liq_owed("alice"_n));
   BOOST_REQUIRE_EQUAL(0, liq_owed(SYND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(success(), liq_claim("alice"_n));
   BOOST_REQUIRE_EQUAL(alice_wire + owed, wire_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(2u, ledger_row(SOLANA, LIQSOL)["queue_epoch"].as<uint32_t>());
   BOOST_REQUIRE(parked_row(LIQSOL, ChainKind::CHAIN_KIND_SVM, pubkey).is_null());
} FC_LOG_AND_RETHROW()

// A syndication whose pubkey has no link at release goes into `parked`, with the WIRE it earned while
// held; the link, when it comes, delivers both.
BOOST_FIXTURE_TEST_CASE(release_parks_an_unlinked_pubkey, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {}));
   BOOST_REQUIRE_EQUAL(success(), liq_mint("bob"_n, LIQSOL, 30 * UNIT));
   syndicate_and_close(1, 1, pubkey, LIQSOL, 30 * UNIT);
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 6 * UNIT));
   const int64_t owed = item_owed(1);
   BOOST_REQUIRE_GT(owed, 0);

   underwrite_envelope(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE(item_row(1).is_null());
   BOOST_REQUIRE_EQUAL(STATE_DONE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   const auto parked = parked_row(LIQSOL, ChainKind::CHAIN_KIND_SVM, pubkey);
   BOOST_REQUIRE(!parked.is_null());
   BOOST_REQUIRE_EQUAL(30 * UNIT, parked["balance"].as_uint64());
   BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(owed), parked["position"]["owed_wire"].as_uint64());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(30 * UNIT), liq_balance(SYND_ACCOUNT));

   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   const int64_t alice_wire = wire_balance("alice"_n);
   BOOST_REQUIRE_EQUAL(success(), sweep("alice"_n, ChainKind::CHAIN_KIND_SVM));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(30 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(owed, liq_owed("alice"_n));
   BOOST_REQUIRE_EQUAL(0, liq_balance(SYND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(success(), liq_claim("alice"_n));
   BOOST_REQUIRE_EQUAL(alice_wire + owed, wire_balance("alice"_n));
} FC_LOG_AND_RETHROW()

// Review Focus 1: the link is resolved at release, not at intake: a pubkey that gains its link while
// its syndication is held is paid to the account, and nothing is parked.
BOOST_FIXTURE_TEST_CASE(a_link_gained_while_held_releases_to_the_account, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {}));
   syndicate_and_close(1, 1, pubkey, LIQSOL, 12 * UNIT);
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));

   underwrite_envelope(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(12 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE(parked_row(LIQSOL, ChainKind::CHAIN_KIND_SVM, pubkey).is_null());
   BOOST_REQUIRE_EQUAL(STATE_DONE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(0, liq_balance(SYND_ACCOUNT));
} FC_LOG_AND_RETHROW()

// Linked contracts receive spendable LIQ without invoking their rejecting transfer handler.
BOOST_FIXTURE_TEST_CASE(a_linked_contract_account_receives_without_notifications, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   BOOST_REQUIRE_EQUAL(success(), link_svm("bob"_n, key));
   set_code("bob"_n, contracts::util::block_transfer_wasm());
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {}));
   syndicate_and_close(1, 1, pubkey, LIQSOL, 9 * UNIT);

   underwrite_envelope(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(success(), crank());
   for (const auto& trace : last_trace->action_traces) BOOST_REQUIRE(trace.receiver != "bob"_n);
   BOOST_REQUIRE_EQUAL(STATE_DONE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(9 * UNIT, liq_balance("bob"_n));
   BOOST_REQUIRE_EQUAL(0, parked_balance(LIQSOL, ChainKind::CHAIN_KIND_SVM, pubkey));
} FC_LOG_AND_RETHROW()

// The syndication bucket starts full at its burst, refills by its refill per depot epoch up to the
// burst, and a tranche is the smaller of an item's remainder and the level. Nothing refills within an
// epoch.
BOOST_FIXTURE_TEST_CASE(bucket_limits_a_tranche_and_refills_per_epoch, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   const auto digest = digest_of("envelope-1");
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.synd_burst = 10 * UNIT, .synd_refill = 4 * UNIT}));
   BOOST_REQUIRE_EQUAL(success(), set_epoch(5));
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 1, ChainKind::CHAIN_KIND_SVM, pubkey, LIQSOL, 8 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 2, ChainKind::CHAIN_KIND_SVM, pubkey, LIQSOL, 8 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), closeenv(SOLANA, 1, digest));
   underwrite_envelope(SOLANA, LIQSOL, 1);

   // A full bucket of 10: the first item whole and 2 of the second.
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(10 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE(item_row(1).is_null());
   BOOST_REQUIRE_EQUAL(6 * UNIT, item_row(2)["remaining"].as_uint64());
   auto bucket = synd_bucket_row(SOLANA, LIQSOL);
   BOOST_REQUIRE_EQUAL(0u, bucket["level"].as_uint64());
   BOOST_REQUIRE_EQUAL(5u, bucket["last_epoch"].as<uint32_t>());
   BOOST_REQUIRE_EQUAL(STATE_RELEASABLE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(10 * UNIT, envelope_row(SOLANA, LIQSOL, 1)["released"].as_uint64());

   // The same epoch: nothing refills and nothing moves.
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE(console_has(QUEUE_EMPTY_BUCKET));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(10 * UNIT), liq_balance("alice"_n));

   // One epoch refills 4.
   BOOST_REQUIRE_EQUAL(success(), set_epoch(6));
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(14 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(2 * UNIT, item_row(2)["remaining"].as_uint64());

   // Three epochs would refill 12, capped at the burst of 10: the last 2 pass and 8 remain.
   BOOST_REQUIRE_EQUAL(success(), set_epoch(9));
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(16 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(STATE_DONE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   bucket = synd_bucket_row(SOLANA, LIQSOL);
   BOOST_REQUIRE_EQUAL(8 * UNIT, bucket["level"].as_uint64());
   BOOST_REQUIRE_EQUAL(9u, bucket["last_epoch"].as<uint32_t>());
} FC_LOG_AND_RETHROW()

// An item larger than the burst never passes whole: it leaves in tranches of at most the burst, one
// bucketful per epoch, until it is gone.
BOOST_FIXTURE_TEST_CASE(oversize_item_releases_in_tranches, sysio_synd_tester) try {
   constexpr uint64_t BURST = 3 * UNIT;
   const auto         key    = ed_key();
   const auto         pubkey = ed_bytes(key);
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.synd_burst = BURST, .synd_refill = BURST}));
   syndicate_and_close(1, 1, pubkey, LIQSOL, 10 * UNIT);
   underwrite_envelope(SOLANA, LIQSOL, 1);

   const std::vector<uint64_t> expected{ BURST, BURST, BURST, UNIT };
   for (uint32_t epoch = 1; epoch <= expected.size(); ++epoch) {
      BOOST_TEST_CONTEXT("epoch " << epoch) {
         BOOST_REQUIRE_EQUAL(success(), set_epoch(epoch));
         const int64_t before = liq_balance("alice"_n);
         BOOST_REQUIRE_EQUAL(success(), crank());
         BOOST_REQUIRE_EQUAL(success(), crank());   // a second step in the same epoch moves nothing
         const int64_t moved = liq_balance("alice"_n) - before;
         BOOST_REQUIRE_EQUAL(static_cast<int64_t>(expected[epoch - 1]), moved);
         BOOST_REQUIRE_LE(moved, static_cast<int64_t>(BURST));
      }
   }
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(10 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(STATE_DONE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(10 * UNIT, envelope_row(SOLANA, LIQSOL, 1)["released"].as_uint64());
} FC_LOG_AND_RETHROW()

// A yield report releases whole into sysio.liq's pending yield, once: no fee, and no budget -- it
// passes while the pair's syndication bucket is shut.
BOOST_FIXTURE_TEST_CASE(yield_item_mints_pending_without_fee_or_budget, sysio_synd_tester) try {
   const auto pubkey = ed_bytes(ed_key());
   const auto digest = digest_of("envelope-1");
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.synd_fee_bps = 1000, .synd_burst = 0,
                                                             .synd_refill = 0}));
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 1, ChainKind::CHAIN_KIND_SVM, pubkey, LIQSOL, 5 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), onyield(SOLANA, 1, digest, 2, 3, LIQSOL, 7 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), closeenv(SOLANA, 1, digest));
   underwrite_envelope(SOLANA, LIQSOL, 1);

   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(7 * UNIT), pending());
   BOOST_REQUIRE_EQUAL(0u, feepot_balance(LIQSOL));
   BOOST_REQUIRE(item_row(2).is_null());
   // The syndication waits for a bucket that does not open.
   BOOST_REQUIRE(console_has(QUEUE_EMPTY_BUCKET));
   BOOST_REQUIRE_EQUAL(5 * UNIT, item_row(1)["remaining"].as_uint64());
   const auto envelope = envelope_row(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(STATE_RELEASABLE, envelope["state"].as_string());
   BOOST_REQUIRE_EQUAL(7 * UNIT, envelope["released"].as_uint64());

   // Released once: the next step mints nothing more.
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(7 * UNIT), pending());
} FC_LOG_AND_RETHROW()

// With no `syndconfig` row the syndication bucket is unset: the step succeeds, prints why, and releases
// nothing. A zero budget is a no-op. Once the pair is configured, the same item releases.
BOOST_FIXTURE_TEST_CASE(crank_never_throws_on_an_unset_bucket, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   syndicate_and_close(1, 1, pubkey, LIQSOL, 10 * UNIT);
   underwrite_envelope(SOLANA, LIQSOL, 1);

   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE(console_has(QUEUE_UNSET_BUCKET));
   BOOST_REQUIRE_EQUAL(0, liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(10 * UNIT, item_row(1)["remaining"].as_uint64());
   BOOST_REQUIRE_EQUAL(STATE_RELEASABLE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE(synd_bucket_row(SOLANA, LIQSOL).is_null());
   BOOST_REQUIRE_EQUAL(success(), crank(0));

   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {}));
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(10 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(STATE_DONE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Configuration and the yield pool
// ---------------------------------------------------------------------------

// `setconfig` is sysio's, for an active outpost and one of its shadowed liq tokens, with fees within a
// whole, a positive window and amounts that fit an asset.
BOOST_FIXTURE_TEST_CASE(setconfig_is_sysio_only_and_validated, sysio_synd_tester) try {
   const pair_config cfg{.synd_fee_bps = 30, .desynd_fee_bps = 40, .synd_burst = 7 * UNIT, .synd_refill = UNIT,
                         .desynd_burst = 8 * UNIT, .desynd_refill = 2 * UNIT, .window_sec = 600,
                         .bounty = 3 * UNIT, .challenge_extra = 4 * UNIT};
   BOOST_REQUIRE(mentions(setconfig(SOLANA, LIQSOL, cfg, "alice"_n), "missing authority of sysio"));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("chain_code is not an active outpost"), setconfig("NOPE", LIQSOL, cfg));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("token_code has no shadow symbol"), setconfig(SOLANA, LIQNONE, cfg));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("token_code belongs to another chain"), setconfig(SOLANA, LIQETH, cfg));
   auto bad = cfg;
   bad.synd_fee_bps = 10001;
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("fee must be at most 10000 basis points"), setconfig(SOLANA, LIQSOL, bad));
   bad = cfg;
   bad.desynd_fee_bps = 10001;
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("fee must be at most 10000 basis points"), setconfig(SOLANA, LIQSOL, bad));
   bad = cfg;
   bad.window_sec = 0;
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("window_sec must be positive"), setconfig(SOLANA, LIQSOL, bad));
   bad = cfg;
   bad.bounty = static_cast<uint64_t>(asset::max_amount) + 1;
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("bucket, bounty and challenge amounts must fit an asset"),
                       setconfig(SOLANA, LIQSOL, bad));
   BOOST_REQUIRE(config_row(SOLANA, LIQSOL).is_null());

   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, cfg));
   const auto row = config_row(SOLANA, LIQSOL);
   BOOST_REQUIRE_EQUAL(30u, row["synd_fee_bps"].as<uint32_t>());
   BOOST_REQUIRE_EQUAL(40u, row["desynd_fee_bps"].as<uint32_t>());
   BOOST_REQUIRE_EQUAL(7 * UNIT, row["synd_burst"].as_uint64());
   BOOST_REQUIRE_EQUAL(UNIT, row["synd_refill"].as_uint64());
   BOOST_REQUIRE_EQUAL(8 * UNIT, row["desynd_burst"].as_uint64());
   BOOST_REQUIRE_EQUAL(2 * UNIT, row["desynd_refill"].as_uint64());
   BOOST_REQUIRE_EQUAL(600u, row["window_sec"].as<uint32_t>());
   BOOST_REQUIRE_EQUAL(3 * UNIT, row["bounty"].as_uint64());
   BOOST_REQUIRE_EQUAL(4 * UNIT, row["challenge_extra"].as_uint64());
   // Another pair keeps no row.
   BOOST_REQUIRE(config_row(SOLANA, LIQTWO).is_null());
} FC_LOG_AND_RETHROW()

// `sweepyield` pulls what sysio.liq owes sysio.synd's row into the token's pool: held items can be paid
// out of it later. Permissionless; a token with no shadow is refused.
BOOST_FIXTURE_TEST_CASE(sweepyield_pulls_what_the_contract_row_earned, sysio_synd_tester) try {
   BOOST_REQUIRE_EQUAL(success(), liq_mint("bob"_n, LIQSOL, 10 * UNIT));
   syndicate_and_close(1, 1, ed_bytes(ed_key()), LIQSOL, 10 * UNIT);
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 4 * UNIT));
   const int64_t owed = liq_owed(SYND_ACCOUNT);
   BOOST_REQUIRE_GT(owed, 0);

   BOOST_REQUIRE_EQUAL(success(), push(SYND_ACCOUNT, synd_abi_ser, "alice"_n, "sweepyield"_n, mvo()
      ("token_code", codename_mvo(LIQSOL))));
   BOOST_REQUIRE_EQUAL(static_cast<uint64_t>(owed), pool_row(LIQSOL)["pool"]["received"].as_uint64());
   BOOST_REQUIRE_EQUAL(0u, pool_row(LIQSOL)["pool"]["credited"].as_uint64());
   BOOST_REQUIRE_EQUAL(owed, wire_balance(SYND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(0, liq_owed(SYND_ACCOUNT));
   // The item is still owed what it earned: the pool now covers it.
   BOOST_REQUIRE_EQUAL(owed, item_owed(1));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("token_code has no shadow symbol"),
                       push(SYND_ACCOUNT, synd_abi_ser, "alice"_n, "sweepyield"_n, mvo()
                          ("token_code", codename_mvo(LIQNONE))));
} FC_LOG_AND_RETHROW()

// The fee pot is protocol revenue, and so is the WIRE it earns: `sweepyield` pays `sysio` what the pot's
// position banked, out of the pool it has just pulled, and nothing is left owed to the pot.
BOOST_FIXTURE_TEST_CASE(sweepyield_pays_the_fee_pot_yield_to_sysio, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.synd_fee_bps = 1000}));
   syndicate_and_close(1, 1, pubkey, LIQSOL, 20 * UNIT);
   underwrite_envelope(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(2 * UNIT, feepot_balance(LIQSOL));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(2 * UNIT), liq_balance(SYND_ACCOUNT));

   // Supply 40 (the syndication 20 and dave's bond 20): 4 WIRE is 0.1 per unit, 0.2 for the pot's 2.
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(40 * UNIT), supply());
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 4 * UNIT));
   const int64_t sysio_wire = wire_balance(SYSIO_ACCOUNT);
   BOOST_REQUIRE_EQUAL(success(), push(SYND_ACCOUNT, synd_abi_ser, "alice"_n, "sweepyield"_n, mvo()
      ("token_code", codename_mvo(LIQSOL))));
   BOOST_REQUIRE_EQUAL(sysio_wire + static_cast<int64_t>(UNIT / 5), wire_balance(SYSIO_ACCOUNT));
   BOOST_REQUIRE_EQUAL(0, wire_balance(SYND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(UNIT / 5, pool_row(LIQSOL)["pool"]["credited"].as_uint64());
   BOOST_REQUIRE_EQUAL(0u, feepot_row(LIQSOL)["position"]["owed_wire"].as_uint64());

   // Nothing more is owed: a second sweep pays nothing.
   BOOST_REQUIRE_EQUAL(success(), push(SYND_ACCOUNT, synd_abi_ser, "alice"_n, "sweepyield"_n, mvo()
      ("token_code", codename_mvo(LIQSOL))));
   BOOST_REQUIRE_EQUAL(sysio_wire + static_cast<int64_t>(UNIT / 5), wire_balance(SYSIO_ACCOUNT));
} FC_LOG_AND_RETHROW()

// The queue records a request's terminal outcome the first time it sees it and never reads the request
// row again, so sysio.bond may prune the row once its ruling is old enough: the pair keeps releasing.
BOOST_FIXTURE_TEST_CASE(a_pruned_request_keeps_its_pair_releasing, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   constexpr uint32_t window_sec = 3600;
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.synd_burst  = 4 * UNIT,
                                                             .synd_refill = 4 * UNIT,
                                                             .window_sec  = window_sec}));
   BOOST_REQUIRE_EQUAL(success(), set_epoch(1));
   syndicate_and_close(1, 1, pubkey, LIQSOL, 10 * UNIT);
   const uint64_t id = envelope_row(SOLANA, LIQSOL, 1)["request_id"].as_uint64();
   underwrite_envelope(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(4 * UNIT), liq_balance("alice"_n));

   // Approved, then seen by a step, which records the outcome.
   produce_block(fc::seconds(window_sec + 1));
   BOOST_REQUIRE_EQUAL(success(), push(BOND_ACCOUNT, bond_abi_ser, "carol"_n, "approve"_n, mvo()("request_id", id)));
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL("APPROVED", envelope_row(SOLANA, LIQSOL, 1)["outcome"].as_string());

   // Dave takes his bond back; once the ruling is a week old nothing keeps the request row.
   BOOST_REQUIRE_EQUAL(success(), bond_claim(id, "dave"_n));
   produce_block(fc::seconds(PRUNE_RETENTION_SEC));
   BOOST_REQUIRE_EQUAL(success(), bond_prune(0, 10));
   BOOST_REQUIRE(bond_request(id).is_null());

   // The envelope goes on releasing from the recorded outcome.
   BOOST_REQUIRE_EQUAL(success(), set_epoch(2));
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(8 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(success(), set_epoch(3));
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(10 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(STATE_DONE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
} FC_LOG_AND_RETHROW()

/// A DONE envelope still consumes a later ruling; a fully paid bond cannot prune that ruling before
/// targeted synchronization, even after the ordinary retention deadline.
BOOST_FIXTURE_TEST_CASE(done_envelope_durably_consumes_late_outcome, sysio_synd_tester) try {
   const auto key = ed_key();
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.synd_burst = 10 * UNIT, .synd_refill = 10 * UNIT,
                                                          .window_sec = 1}));
   syndicate_and_close(1, 1, ed_bytes(key), LIQSOL, UNIT);
   const auto id = envelope_row(SOLANA, LIQSOL, 1)["request_id"].as_uint64();
   underwrite_envelope(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(STATE_DONE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   produce_block(fc::seconds(2));
   BOOST_REQUIRE_EQUAL(success(), push(BOND_ACCOUNT, bond_abi_ser, "carol"_n, "approve"_n, mvo()("request_id", id)));
   BOOST_REQUIRE_EQUAL(success(), bond_claim(id, "dave"_n));
   produce_block(fc::seconds(PRUNE_RETENTION_SEC + 1));
   BOOST_REQUIRE_EQUAL(success(), bond_prune(id, 1));
   BOOST_REQUIRE(!bond_request(id).is_null());
   BOOST_REQUIRE_EQUAL(success(), push(SYND_ACCOUNT, synd_abi_ser, "carol"_n, "syncenv"_n, mvo()
      ("chain_code", codename_mvo(SOLANA))("token_code", codename_mvo(LIQSOL))("epoch_index", 1)));
   BOOST_REQUIRE_EQUAL("APPROVED", envelope_row(SOLANA, LIQSOL, 1)["outcome"].as_string());
   BOOST_REQUIRE_EQUAL(STATE_DONE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(success(), bond_prune(id, 1));
   BOOST_REQUIRE(bond_request(id).is_null());
} FC_LOG_AND_RETHROW()

/// Governance may invalidate a fully released envelope without a prior challenge. Synchronization
/// must rewind settlement so the forfeit covers the released amount exactly once.
BOOST_FIXTURE_TEST_CASE(late_invalid_outcome_rewinds_done_envelope, sysio_synd_tester) try {
   const auto key = ed_key();
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {}));
   syndicate_and_close(1, 1, ed_bytes(key), LIQSOL, UNIT);
   const auto id = envelope_row(SOLANA, LIQSOL, 1)["request_id"].as_uint64();
   underwrite_envelope(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(STATE_DONE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(success(), rule(id, false));
   BOOST_REQUIRE_EQUAL(success(), push(SYND_ACCOUNT, synd_abi_ser, "carol"_n, "syncenv"_n, mvo()
      ("chain_code", codename_mvo(SOLANA))("token_code", codename_mvo(LIQSOL))("epoch_index", 1)));
   BOOST_REQUIRE_EQUAL(1u, ledger_row(SOLANA, LIQSOL)["queue_epoch"].as_uint64());
   BOOST_REQUIRE_EQUAL(0u, envelope_row(SOLANA, LIQSOL, 1)["burned"].as_uint64());
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(STATE_INVALID, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(UNIT, envelope_row(SOLANA, LIQSOL, 1)["burned"].as_uint64());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(UNIT, envelope_row(SOLANA, LIQSOL, 1)["burned"].as_uint64());
} FC_LOG_AND_RETHROW()

/// Fully released is not final. Pruning requires consumed finality, then retains a replay floor
/// that rejects both old and fresh sequences at erased epochs, even with a different digest.
BOOST_FIXTURE_TEST_CASE(prune_requires_finality_and_preserves_replay_floor, sysio_synd_tester) try {
   const auto key = ed_key();
   const auto pubkey = ed_bytes(key);
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {}));
   syndicate_and_close(1, 1, pubkey, LIQSOL, UNIT);
   const auto id = envelope_row(SOLANA, LIQSOL, 1)["request_id"].as_uint64();
   underwrite_envelope(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(success(), crank());
   const auto prune = [&] { return push(SYND_ACCOUNT, synd_abi_ser, "carol"_n, "pruneenv"_n, mvo()
      ("chain_code", codename_mvo(SOLANA))("token_code", codename_mvo(LIQSOL))("limit", 1)); };
   BOOST_REQUIRE_EQUAL(success(), prune());
   BOOST_REQUIRE(!envelope_row(SOLANA, LIQSOL, 1).is_null());
   BOOST_REQUIRE_EQUAL(success(), rule(id, true));
   BOOST_REQUIRE_EQUAL(success(), prune()); // final bond outcome has not been consumed yet
   BOOST_REQUIRE(!envelope_row(SOLANA, LIQSOL, 1).is_null());
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(success(), prune());
   BOOST_REQUIRE(envelope_row(SOLANA, LIQSOL, 1).is_null());
   BOOST_REQUIRE_EQUAL(2u, ledger_row(SOLANA, LIQSOL)["retained_from_epoch"].as_uint64());
   const auto before = supply();
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest_of("changed"), 2, ChainKind::CHAIN_KIND_SVM,
                                         pubkey, LIQSOL, UNIT));
   BOOST_REQUIRE_EQUAL(success(), onyield(SOLANA, 1, digest_of("changed"), 2, 2, LIQSOL, UNIT));
   BOOST_REQUIRE_EQUAL(success(), closeenv(SOLANA, 1, digest_of("changed")));
   BOOST_REQUIRE(envelope_row(SOLANA, LIQSOL, 1).is_null());
   BOOST_REQUIRE_EQUAL(before, supply());
   BOOST_REQUIRE_EQUAL(1u, cursor_row(SOLANA)["last_sequence"].as_uint64());
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 2, digest_of("new"), 2, ChainKind::CHAIN_KIND_SVM,
                                         pubkey, LIQSOL, UNIT));
   BOOST_REQUIRE(!envelope_row(SOLANA, LIQSOL, 2).is_null());
} FC_LOG_AND_RETHROW()

/// Compaction is bounded and prefix-only; unresolved earlier envelopes protect later replay markers.
BOOST_FIXTURE_TEST_CASE(prune_stops_at_open_prefix_and_respects_limit, sysio_synd_tester) try {
   const auto pubkey = ed_bytes(ed_key());
   for (uint32_t n = 1; n <= 3; ++n)
      BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, n, digest_of(std::to_string(n)), n,
                                            ChainKind::CHAIN_KIND_SVM, pubkey, LIQSOL, UNIT));
   BOOST_REQUIRE_EQUAL(success(), dropenv(SOLANA, LIQSOL, 2));
   BOOST_REQUIRE_EQUAL(success(), dropenv(SOLANA, LIQSOL, 3));
   const auto prune = [&](uint32_t limit) { return push(SYND_ACCOUNT, synd_abi_ser, "carol"_n, "pruneenv"_n, mvo()
      ("chain_code", codename_mvo(SOLANA))("token_code", codename_mvo(LIQSOL))("limit", limit)); };
   BOOST_REQUIRE_EQUAL(success(), prune(10));
   BOOST_REQUIRE(!envelope_row(SOLANA, LIQSOL, 2).is_null());
   BOOST_REQUIRE_EQUAL(success(), dropenv(SOLANA, LIQSOL, 1));
   BOOST_REQUIRE_EQUAL(success(), prune(0));
   BOOST_REQUIRE(!envelope_row(SOLANA, LIQSOL, 1).is_null());
   BOOST_REQUIRE_EQUAL(success(), prune(1));
   BOOST_REQUIRE(envelope_row(SOLANA, LIQSOL, 1).is_null());
   BOOST_REQUIRE(!envelope_row(SOLANA, LIQSOL, 2).is_null());
   BOOST_REQUIRE_EQUAL(success(), prune(2));
   BOOST_REQUIRE(envelope_row(SOLANA, LIQSOL, 2).is_null());
   BOOST_REQUIRE(envelope_row(SOLANA, LIQSOL, 3).is_null());
   BOOST_REQUIRE_EQUAL(4u, ledger_row(SOLANA, LIQSOL)["retained_from_epoch"].as_uint64());
} FC_LOG_AND_RETHROW()

/// Tiny independent synchronization sweeps reach the tail of an empty-bucket backlog without a
/// larger release budget. Issuance progresses past each fully bonded request.
BOOST_FIXTURE_TEST_CASE(small_sync_sweeps_progress_past_blocked_release, sysio_synd_tester) try {
   const auto pubkey = ed_bytes(ed_key());
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.synd_burst = 0, .synd_refill = 0}));
   for (uint32_t n = 1; n <= 20; ++n) {
      syndicate_and_close(n, n, pubkey, LIQSOL, UNIT);
      BOOST_REQUIRE_EQUAL(STATE_REQUESTED, envelope_row(SOLANA, LIQSOL, n)["state"].as_string());
      underwrite_envelope(SOLANA, LIQSOL, n);
   }
   const auto id = envelope_row(SOLANA, LIQSOL, 20)["request_id"].as_uint64();
   BOOST_REQUIRE_EQUAL(success(), rule(id, false));
   for (unsigned n = 0; n < 21; ++n)
      BOOST_REQUIRE_EQUAL(success(), push(SYND_ACCOUNT, synd_abi_ser, "carol"_n, "sync"_n, mvo()("limit", 1)));
   BOOST_REQUIRE_EQUAL("INVALID", envelope_row(SOLANA, LIQSOL, 20)["outcome"].as_string());
   BOOST_REQUIRE_EQUAL(0u, envelope_row(SOLANA, LIQSOL, 20)["burned"].as_uint64());
} FC_LOG_AND_RETHROW()

// A pair that can move nothing more costs the rest of the step nothing: 17 held syndications of a token
// with no syndconfig row do not starve the other token of closeenv's small inline step, which still
// requests it and, once bonded, releases it.
BOOST_FIXTURE_TEST_CASE(a_stopped_pair_leaves_the_budget_to_the_others, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQTWO, {}));
   const auto digest1 = digest_of("envelope-1");
   for (uint64_t sequence = 1; sequence <= 17; ++sequence) {
      BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest1, sequence, ChainKind::CHAIN_KIND_SVM, pubkey, LIQSOL,
                                            UNIT));
   }
   BOOST_REQUIRE_EQUAL(success(), closeenv(SOLANA, 1, digest1));
   underwrite_envelope(SOLANA, LIQSOL, 1);

   // Epoch 2 carries LIQTWO: closeenv's step walks LIQSOL first, finds its bucket unset and leaves it.
   syndicate_and_close(2, 18, pubkey, LIQTWO, 3 * UNIT);
   BOOST_REQUIRE(console_has(QUEUE_UNSET_BUCKET));
   BOOST_REQUIRE_EQUAL(STATE_RELEASABLE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(STATE_REQUESTED, envelope_row(SOLANA, LIQTWO, 2)["state"].as_string());

   // Bonded, it is released by the next envelope's inline step, the 17 LIQSOL items still held.
   underwrite_envelope(SOLANA, LIQTWO, 2);
   syndicate_and_close(3, 19, pubkey, LIQSOL, UNIT);
   BOOST_REQUIRE_EQUAL(STATE_DONE, envelope_row(SOLANA, LIQTWO, 2)["state"].as_string());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(3 * UNIT), liq_balance("alice"_n, LIQTWO_SYM));
   BOOST_REQUIRE_EQUAL(0, liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(17u, envelope_row(SOLANA, LIQSOL, 1)["item_count"].as<uint32_t>());
   BOOST_REQUIRE(!item_row(17).is_null());
} FC_LOG_AND_RETHROW()

// Envelopes stuck behind an empty bucket cost closeenv's small inline step nothing: 17 RELEASABLE LIQSOL
// envelopes, one syndication each and no yield, do not stop the same step requesting a LIQTWO envelope
// and, once it is bonded, releasing it.
BOOST_FIXTURE_TEST_CASE(envelopes_behind_an_empty_bucket_leave_closeenv_its_budget, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.synd_burst = 0, .synd_refill = 0}));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQTWO, {}));
   constexpr uint32_t stuck = 17;
   for (uint32_t epoch = 1; epoch <= stuck; ++epoch) {
      syndicate_and_close(epoch, epoch, pubkey, LIQSOL, UNIT);
      BOOST_REQUIRE_EQUAL(success(), crank());
      underwrite_envelope(SOLANA, LIQSOL, epoch);
   }
   BOOST_REQUIRE_EQUAL(success(), crank());
   for (uint32_t epoch = 1; epoch <= stuck; ++epoch) {
      BOOST_REQUIRE_EQUAL(STATE_RELEASABLE, envelope_row(SOLANA, LIQSOL, epoch)["state"].as_string());
   }
   BOOST_REQUIRE(console_has(QUEUE_EMPTY_BUCKET));

   // One closeenv step walks LIQSOL first and still requests the LIQTWO envelope.
   syndicate_and_close(stuck + 1, stuck + 1, pubkey, LIQTWO, 3 * UNIT);
   BOOST_REQUIRE_EQUAL(STATE_REQUESTED, envelope_row(SOLANA, LIQTWO, stuck + 1)["state"].as_string());

   // Bonded, it is released by the next envelope's inline step.
   underwrite_envelope(SOLANA, LIQTWO, stuck + 1);
   syndicate_and_close(stuck + 2, stuck + 2, pubkey, LIQSOL, UNIT);
   BOOST_REQUIRE_EQUAL(STATE_DONE, envelope_row(SOLANA, LIQTWO, stuck + 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(3 * UNIT), liq_balance("alice"_n, LIQTWO_SYM));
   BOOST_REQUIRE_EQUAL(0, liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(STATE_RELEASABLE, envelope_row(SOLANA, LIQSOL, stuck)["state"].as_string());

   // A step whose budget runs out on LIQSOL, which moves nothing, leaves the next step to start past it.
   BOOST_REQUIRE_EQUAL(success(), crank(1));
   BOOST_REQUIRE_EQUAL(LIQTWO_SYM.to_symbol_code().value, state_row()["queue_cursor"].as_uint64());
} FC_LOG_AND_RETHROW()

// The examination cap bounds a step whatever the pairs hold: six pairs (LIQA-LIQE, LIQSOL), each with a
// A blocked release pass retains its examination cap and cursor. Underwriting has an independent
// budget and reaches a later pair immediately, even when release exhausts its own budget first.
BOOST_FIXTURE_TEST_CASE(the_examination_cap_bounds_a_step_over_many_blocked_pairs, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   // In queue order: the four-letter shadows sort before every six-letter one, LIQTWO last.
   const std::vector<std::string> blocked{"LIQA", "LIQB", "LIQC", "LIQD", "LIQE", std::string(LIQSOL)};
   constexpr uint32_t             items = 16;
   uint64_t                       sequence = 0;
   uint32_t                       epoch    = 0;
   for (size_t i = 0; i < blocked.size(); ++i) {
      const std::string& code = blocked[i];
      if (code != LIQSOL) {
         BOOST_REQUIRE_EQUAL(success(), regtoken(code, 9, ChainKind::CHAIN_KIND_SVM, SOLANA,
                                                 std::vector<char>(32, char(0x60 + i))));
         BOOST_REQUIRE_EQUAL(success(), create_shadow(symbol::from_string("9," + code), SOLANA, code));
      }
      BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, code, {.synd_burst = 0, .synd_refill = 0}));
      ++epoch;
      const auto digest = digest_of("envelope-" + std::to_string(epoch));
      for (uint32_t item = 0; item < items; ++item) {
         BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, epoch, digest, ++sequence, ChainKind::CHAIN_KIND_SVM, pubkey,
                                               code, UNIT));
      }
      BOOST_REQUIRE_EQUAL(success(), closeenv(SOLANA, epoch, digest));
      BOOST_REQUIRE_EQUAL(STATE_REQUESTED, envelope_row(SOLANA, code, epoch)["state"].as_string());
   }
   // Bonded only now: while requested, a pair costs the setup steps one examination, so none ran out
   // and the cursor was never written: steps start at the first pair.
   for (uint32_t e = 1; e <= epoch; ++e) underwrite_envelope(SOLANA, blocked[e - 1], e);
   BOOST_REQUIRE(state_row().is_null());
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQTWO, {}));

   // Release stops in LIQD, while the independent issuance pass reaches LIQTWO.
   const uint32_t two = ++epoch;
   syndicate_and_close(two, ++sequence, pubkey, LIQTWO, 3 * UNIT);
   BOOST_REQUIRE_EQUAL(STATE_REQUESTED, envelope_row(SOLANA, LIQTWO, two)["state"].as_string());
   BOOST_REQUIRE_EQUAL(symbol::from_string("9,LIQE").to_symbol_code().value,
                       state_row()["queue_cursor"].as_uint64());
   underwrite_envelope(SOLANA, LIQTWO, two);
   // Repeated bounded steps eventually deliver that pair without draining any empty-bucket pair.
   for (unsigned step = 0; step < 8 &&
        envelope_row(SOLANA, LIQTWO, two)["state"].as_string() != STATE_DONE; ++step)
      BOOST_REQUIRE_EQUAL(success(), crank(16));
   BOOST_REQUIRE_EQUAL(STATE_DONE, envelope_row(SOLANA, LIQTWO, two)["state"].as_string());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(3 * UNIT), liq_balance("alice"_n, LIQTWO_SYM));
   for (uint32_t e = 1; e <= blocked.size(); ++e) {
      BOOST_REQUIRE_EQUAL(STATE_RELEASABLE, envelope_row(SOLANA, blocked[e - 1], e)["state"].as_string());
      BOOST_REQUIRE_EQUAL(UNIT, item_row((e - 1) * items + 1)["remaining"].as_uint64());
   }
} FC_LOG_AND_RETHROW()

// Idle pairs cost the step too: with nothing admitted for seven shadows ahead of it (LIQA-LIQE, LIQETH and
// LIQSOL), a crank(1) -- budget 1, cap 4 -- pays one entry look per pair. From the start of the queue it
// ends on its cap at LIQD without reaching the bonded LIQTWO envelope; the next ends on LIQTWO's own entry
// look (LIQE, LIQETH, LIQSOL, LIQTWO) before it can look at the envelope, so the cursor stays on LIQTWO;
// the third starts there and releases it (entry, envelope, item: three looks and the one unit).
BOOST_FIXTURE_TEST_CASE(a_step_over_many_idle_pairs_stops_on_the_cap, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   const std::vector<std::string> idle{"LIQA", "LIQB", "LIQC", "LIQD", "LIQE"};
   for (size_t i = 0; i < idle.size(); ++i) {
      BOOST_REQUIRE_EQUAL(success(), regtoken(idle[i], 9, ChainKind::CHAIN_KIND_SVM, SOLANA,
                                              std::vector<char>(32, char(0x60 + i))));
      BOOST_REQUIRE_EQUAL(success(), create_shadow(symbol::from_string("9," + idle[i]), SOLANA, idle[i]));
   }
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQTWO, {}));
   syndicate_and_close(1, 1, pubkey, LIQTWO, 3 * UNIT);
   underwrite_envelope(SOLANA, LIQTWO, 1);
   BOOST_REQUIRE_EQUAL(STATE_REQUESTED, envelope_row(SOLANA, LIQTWO, 1)["state"].as_string());
   BOOST_REQUIRE(state_row().is_null());

   BOOST_REQUIRE_EQUAL(success(), crank(1));
   BOOST_REQUIRE_EQUAL(STATE_REQUESTED, envelope_row(SOLANA, LIQTWO, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(symbol::from_string("9,LIQE").to_symbol_code().value,
                       state_row()["queue_cursor"].as_uint64());

   BOOST_REQUIRE_EQUAL(success(), crank(1));
   BOOST_REQUIRE_EQUAL(STATE_REQUESTED, envelope_row(SOLANA, LIQTWO, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(LIQTWO_SYM.to_symbol_code().value, state_row()["queue_cursor"].as_uint64());

   BOOST_REQUIRE_EQUAL(success(), crank(1));
   BOOST_REQUIRE_EQUAL(STATE_DONE, envelope_row(SOLANA, LIQTWO, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(3 * UNIT), liq_balance("alice"_n, LIQTWO_SYM));
} FC_LOG_AND_RETHROW()

// A pair both of whose flows have stopped -- its syndications behind an empty bucket, its yield behind a
// full headroom -- still has a later envelope requested and its request's outcome recorded, so a prune of
// the request cannot strand the envelope.
BOOST_FIXTURE_TEST_CASE(a_pair_with_both_flows_stopped_records_a_later_outcome, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   constexpr uint32_t window_sec = 3600;
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.synd_burst = 0, .synd_refill = 0,
                                                             .window_sec = window_sec}));
   // Envelope 1: a syndication the empty bucket holds.
   syndicate_and_close(1, 1, pubkey, LIQSOL, UNIT);
   underwrite_envelope(SOLANA, LIQSOL, 1);
   // Envelope 2: a yield report, requested past envelope 1 and bonded.
   const auto digest2 = digest_of("envelope-2");
   BOOST_REQUIRE_EQUAL(success(), onyield(SOLANA, 2, digest2, 2, 2, LIQSOL, 5 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), closeenv(SOLANA, 2, digest2));
   BOOST_REQUIRE_EQUAL(STATE_REQUESTED, envelope_row(SOLANA, LIQSOL, 2)["state"].as_string());
   underwrite_envelope(SOLANA, LIQSOL, 2);

   // The supply is taken to 4 UNIT under the range: once envelope 3 mints one more, the 5 of yield no
   // longer fit, and both flows of the pair stop at envelopes 1 and 2.
   const uint64_t range = static_cast<uint64_t>(asset::max_amount);
   BOOST_REQUIRE_EQUAL(success(), liq_mint(SYND_ACCOUNT, LIQSOL, range - static_cast<uint64_t>(supply()) - 4 * UNIT));
   syndicate_and_close(3, 3, pubkey, LIQSOL, UNIT);
   BOOST_REQUIRE(console_has(QUEUE_EMPTY_BUCKET));
   BOOST_REQUIRE(console_has(QUEUE_YIELD_HEADROOM));
   BOOST_REQUIRE_EQUAL(STATE_REQUESTED, envelope_row(SOLANA, LIQSOL, 3)["state"].as_string());
   const uint64_t id = envelope_row(SOLANA, LIQSOL, 3)["request_id"].as_uint64();
   underwrite_envelope(SOLANA, LIQSOL, 3);

   // Approved; a step with both flows still stopped records the outcome.
   produce_block(fc::seconds(window_sec + 1));
   BOOST_REQUIRE_EQUAL(success(), push(BOND_ACCOUNT, bond_abi_ser, "carol"_n, "approve"_n, mvo()("request_id", id)));
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE(console_has(QUEUE_EMPTY_BUCKET));
   BOOST_REQUIRE(console_has(QUEUE_YIELD_HEADROOM));
   BOOST_REQUIRE_EQUAL("APPROVED", envelope_row(SOLANA, LIQSOL, 3)["outcome"].as_string());
   BOOST_REQUIRE_EQUAL(STATE_RELEASABLE, envelope_row(SOLANA, LIQSOL, 3)["state"].as_string());

   // Pruned a week later, the request no longer holds the envelope: it stays RELEASABLE on its record.
   BOOST_REQUIRE_EQUAL(success(), bond_claim(id, "dave"_n));
   produce_block(fc::seconds(PRUNE_RETENTION_SEC));
   BOOST_REQUIRE_EQUAL(success(), bond_prune(0, 10));
   BOOST_REQUIRE(bond_request(id).is_null());
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(STATE_RELEASABLE, envelope_row(SOLANA, LIQSOL, 3)["state"].as_string());
   BOOST_REQUIRE_EQUAL(0, liq_balance("alice"_n));
} FC_LOG_AND_RETHROW()

// A HELD envelope whose request was fully bonded holds only itself: a later envelope of the pair is
// requested and released past it, and the pair's queue cursor stays on the held one.
BOOST_FIXTURE_TEST_CASE(a_held_envelope_is_skipped_while_a_later_one_releases, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {}));
   syndicate_and_close(1, 1, pubkey, LIQSOL, 10 * UNIT);
   const uint64_t first = envelope_row(SOLANA, LIQSOL, 1)["request_id"].as_uint64();
   underwrite_envelope(SOLANA, LIQSOL, 1);
   // Held by its issuer before any step saw the bond.
   BOOST_REQUIRE_EQUAL(success(), bond_hold(first, "carol"_n));
   BOOST_REQUIRE_EQUAL(REQUEST_HELD, bond_request(first)["state"].as_string());

   syndicate_and_close(2, 2, pubkey, LIQSOL, 4 * UNIT);
   BOOST_REQUIRE_EQUAL(STATE_HELD, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(STATE_REQUESTED, envelope_row(SOLANA, LIQSOL, 2)["state"].as_string());
   underwrite_envelope(SOLANA, LIQSOL, 2);
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(STATE_HELD, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(STATE_DONE, envelope_row(SOLANA, LIQSOL, 2)["state"].as_string());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(4 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(10 * UNIT, item_row(1)["remaining"].as_uint64());
   BOOST_REQUIRE_EQUAL(0u, ledger_row(SOLANA, LIQSOL)["queue_epoch"].as<uint32_t>());
} FC_LOG_AND_RETHROW()

// A yield release the shadow's headroom cannot take waits, and a later step releases it once the headroom
// has room again.
BOOST_FIXTURE_TEST_CASE(a_yield_release_waits_for_headroom_and_retries, sysio_synd_tester) try {
   const auto digest = digest_of("envelope-1");
   BOOST_REQUIRE_EQUAL(success(), onyield(SOLANA, 1, digest, 1, 3, LIQSOL, 5 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), closeenv(SOLANA, 1, digest));
   underwrite_envelope(SOLANA, LIQSOL, 1);

   // The supply is taken to 4 UNIT under the range: the 5 of yield no longer fit.
   const uint64_t range = static_cast<uint64_t>(asset::max_amount);
   BOOST_REQUIRE_EQUAL(success(), liq_mint(SYND_ACCOUNT, LIQSOL, range - static_cast<uint64_t>(supply()) - 4 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE(console_has(QUEUE_YIELD_HEADROOM));
   BOOST_REQUIRE_EQUAL(0, pending());
   BOOST_REQUIRE_EQUAL(STATE_RELEASABLE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(5 * UNIT, item_row(1)["remaining"].as_uint64());

   // One more unit of room: the next step releases it whole.
   BOOST_REQUIRE_EQUAL(success(), liq_burn(LIQSOL, UNIT));
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(5 * UNIT), pending());
   BOOST_REQUIRE(item_row(1).is_null());
   BOOST_REQUIRE_EQUAL(STATE_DONE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
} FC_LOG_AND_RETHROW()

// `dropenv` is sysio's escape hatch for an envelope with no request: an OPEN or WAITING one has its held
// syndications burned and its yield reports dropped, is INVALID, and the queue moves past it. A requested
// envelope, a missing one and another signer are refused.
BOOST_FIXTURE_TEST_CASE(dropenv_burns_an_unrequested_envelope_and_the_queue_moves_on, sysio_synd_tester) try {
   const auto pubkey = ed_bytes(ed_key());
   constexpr auto SVM = ChainKind::CHAIN_KIND_SVM;
   // Epoch 1 is requested; epoch 2 waits behind it; epoch 3 is still OPEN.
   syndicate_and_close(1, 1, pubkey, LIQSOL, 10 * UNIT);
   const auto digest2 = digest_of("envelope-2");
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 2, digest2, 2, SVM, pubkey, LIQSOL, 4 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), onyield(SOLANA, 2, digest2, 3, 7, LIQSOL, 2 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), closeenv(SOLANA, 2, digest2));
   const auto digest3 = digest_of("envelope-3");
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 3, digest3, 4, SVM, pubkey, LIQSOL, 3 * UNIT));
   BOOST_REQUIRE_EQUAL(STATE_WAITING, envelope_row(SOLANA, LIQSOL, 2)["state"].as_string());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(17 * UNIT), supply());

   BOOST_REQUIRE(mentions(dropenv(SOLANA, LIQSOL, 2, "carol"_n), "missing authority of sysio"));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg(std::string(DROPENV_STATE)), dropenv(SOLANA, LIQSOL, 1));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("envelope not found"), dropenv(SOLANA, LIQSOL, 9));

   BOOST_REQUIRE_EQUAL(success(), dropenv(SOLANA, LIQSOL, 2));
   auto envelope = envelope_row(SOLANA, LIQSOL, 2);
   BOOST_REQUIRE_EQUAL(STATE_INVALID, envelope["state"].as_string());
   BOOST_REQUIRE_EQUAL(4 * UNIT, envelope["burned"].as_uint64());
   BOOST_REQUIRE(item_row(2).is_null());
   BOOST_REQUIRE(item_row(3).is_null());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(13 * UNIT), supply());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(13 * UNIT), liq_balance(SYND_ACCOUNT));

   BOOST_REQUIRE_EQUAL(success(), dropenv(SOLANA, LIQSOL, 3));
   BOOST_REQUIRE_EQUAL(STATE_INVALID, envelope_row(SOLANA, LIQSOL, 3)["state"].as_string());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(10 * UNIT), supply());
   // A late message of the dropped OPEN envelope is dropped too.
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 3, digest3, 5, SVM, pubkey, LIQSOL, UNIT));
   BOOST_REQUIRE(console_has(DROP_CLOSED));

   // Once epoch 1 is released the queue moves past both dropped envelopes.
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {}));
   underwrite_envelope(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(STATE_DONE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(4u, ledger_row(SOLANA, LIQSOL)["queue_epoch"].as<uint32_t>());
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// Challenges and the invalid envelope
// ---------------------------------------------------------------------------

// A challenge charges the challenger the request's hold bond plus the pair's extra: the extra stays in
// the fee pot, sysio.bond pulls the hold bond back out of sysio.synd as the issuer's hold naming the
// challenger, and the envelope is HELD. It is the challenger's own action, and one per request.
BOOST_FIXTURE_TEST_CASE(challenge_charges_bond_plus_extra_and_holds, sysio_synd_tester) try {
   const auto pubkey = ed_bytes(ed_key());
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.challenge_extra = 2 * UNIT}));
   syndicate_and_close(1, 1, pubkey, LIQSOL, 10 * UNIT);
   const uint64_t id = envelope_row(SOLANA, LIQSOL, 1)["request_id"].as_uint64();
   BOOST_REQUIRE_EQUAL(success(), liq_mint("carol"_n, LIQSOL, 5 * UNIT));

   BOOST_REQUIRE(mentions(challenge("carol"_n, SOLANA, LIQSOL, 1, "bob"_n), "missing authority of carol"));

   BOOST_REQUIRE_EQUAL(success(), challenge("carol"_n, SOLANA, LIQSOL, 1));
   const uint64_t hold_bond = 10 * UNIT * HOLD_BPS / BPS_DENOMINATOR;
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(5 * UNIT - hold_bond - 2 * UNIT), liq_balance("carol"_n));
   BOOST_REQUIRE_EQUAL(2 * UNIT, feepot_balance(LIQSOL));
   // sysio.synd keeps the held syndication and the extra; sysio.bond holds the hold bond.
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(12 * UNIT), liq_balance(SYND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(hold_bond), liq_balance(BOND_ACCOUNT));
   const auto req = bond_request(id);
   BOOST_REQUIRE_EQUAL(REQUEST_HELD, req["state"].as_string());
   BOOST_REQUIRE_EQUAL(hold_bond, req["hold_bond"].as_uint64());
   BOOST_REQUIRE_EQUAL("carol", req["hold_beneficiary"].as_string());
   BOOST_REQUIRE_EQUAL(STATE_HELD, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());

   // One challenge per request: a second is refused and charges nothing.
   BOOST_REQUIRE(mentions(challenge("carol"_n, SOLANA, LIQSOL, 1), CHALLENGE_TWICE));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(2 * UNIT), liq_balance("carol"_n));

   // The step keeps the envelope HELD and nothing is released.
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(STATE_HELD, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(10 * UNIT, item_row(1)["remaining"].as_uint64());
} FC_LOG_AND_RETHROW()

// Review Focus 4: a request that is APPROVED can no longer be challenged, even while its envelope is
// still RELEASABLE (here its bucket is shut) or not yet refreshed; nothing is charged.
BOOST_FIXTURE_TEST_CASE(challenge_refused_on_an_approved_request, sysio_synd_tester) try {
   constexpr uint32_t WINDOW_SEC = 1;
   const auto         pubkey     = ed_bytes(ed_key());
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.synd_burst = 0, .synd_refill = 0,
                                                             .window_sec = WINDOW_SEC}));
   syndicate_and_close(1, 1, pubkey, LIQSOL, 10 * UNIT);
   const uint64_t id = envelope_row(SOLANA, LIQSOL, 1)["request_id"].as_uint64();
   underwrite_envelope(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(success(), liq_mint("carol"_n, LIQSOL, 5 * UNIT));
   produce_block(fc::seconds(WINDOW_SEC + 1));
   BOOST_REQUIRE_EQUAL(success(), push(BOND_ACCOUNT, bond_abi_ser, "carol"_n, "approve"_n, mvo()("request_id", id)));
   BOOST_REQUIRE_EQUAL(REQUEST_APPROVED, bond_request(id)["state"].as_string());

   // Not yet refreshed: the envelope is REQUESTED, the request APPROVED.
   BOOST_REQUIRE_EQUAL(STATE_REQUESTED, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE(mentions(challenge("carol"_n, SOLANA, LIQSOL, 1), CHALLENGE_FINAL));

   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(STATE_RELEASABLE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE(mentions(challenge("carol"_n, SOLANA, LIQSOL, 1), CHALLENGE_FINAL));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(5 * UNIT), liq_balance("carol"_n));
   BOOST_REQUIRE_EQUAL(REQUEST_APPROVED, bond_request(id)["state"].as_string());
} FC_LOG_AND_RETHROW()

// Review Focus 4: an envelope with no request issued yet (WAITING behind an unbonded one), or with no
// row at all, cannot be challenged; nor can sysio.synd, sysio.bond or an account with contract code
// challenge.
BOOST_FIXTURE_TEST_CASE(challenge_refused_on_an_unrequested_envelope, sysio_synd_tester) try {
   const auto pubkey = ed_bytes(ed_key());
   syndicate_and_close(1, 1, pubkey, LIQSOL, 10 * UNIT);
   syndicate_and_close(2, 2, pubkey, LIQSOL, 4 * UNIT);
   BOOST_REQUIRE_EQUAL(STATE_WAITING, envelope_row(SOLANA, LIQSOL, 2)["state"].as_string());
   BOOST_REQUIRE_EQUAL(success(), liq_mint("carol"_n, LIQSOL, 5 * UNIT));

   BOOST_REQUIRE(mentions(challenge("carol"_n, SOLANA, LIQSOL, 2), CHALLENGE_STATE));
   BOOST_REQUIRE(mentions(challenge("carol"_n, SOLANA, LIQSOL, 9), CHALLENGE_NO_ROW));
   BOOST_REQUIRE(mentions(challenge(SYND_ACCOUNT, SOLANA, LIQSOL, 1), CHALLENGE_ROLE));
   BOOST_REQUIRE_EQUAL(success(), liq_mint("bob"_n, LIQSOL, 5 * UNIT));
   set_code("bob"_n, contracts::util::block_transfer_wasm());
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), challenge("bob"_n, SOLANA, LIQSOL, 1));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(5 * UNIT), liq_balance("carol"_n));
   BOOST_REQUIRE_EQUAL(REQUEST_HELD, bond_request(1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(0u, feepot_balance(LIQSOL));
} FC_LOG_AND_RETHROW()

/// Half-released envelope: alice is linked, the bucket passes 4 per epoch, 10 are syndicated at epoch 1,
/// bonded, and one step releases 4. Carol holds 5 to challenge with.
struct sysio_synd_released_tester : sysio_synd_tester {
   sysio_synd_released_tester() {
      const auto key = ed_key();
      pubkey         = ed_bytes(key);
      BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
      BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.synd_burst = 4 * UNIT, .synd_refill = 4 * UNIT}));
      BOOST_REQUIRE_EQUAL(success(), set_epoch(1));
      syndicate_and_close(1, 1, pubkey, LIQSOL, 10 * UNIT);
      id = envelope_row(SOLANA, LIQSOL, 1)["request_id"].as_uint64();
      underwrite_envelope(SOLANA, LIQSOL, 1);
      BOOST_REQUIRE_EQUAL(success(), crank());
      BOOST_REQUIRE_EQUAL(static_cast<int64_t>(4 * UNIT), liq_balance("alice"_n));
      BOOST_REQUIRE_EQUAL(STATE_RELEASABLE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
      BOOST_REQUIRE_EQUAL(success(), liq_mint("carol"_n, LIQSOL, 5 * UNIT));
   }
   std::vector<char> pubkey;
   uint64_t          id = 0;
};

// Review Focus 4: a RELEASABLE envelope whose request is still BONDED can be challenged after part of it
// was released; the challenge stops the rest, even with the bucket refilled.
BOOST_FIXTURE_TEST_CASE(a_challenge_after_partial_release_stops_further_release, sysio_synd_released_tester) try {
   BOOST_REQUIRE_EQUAL(REQUEST_BONDED, bond_request(id)["state"].as_string());
   BOOST_REQUIRE_EQUAL(success(), challenge("carol"_n, SOLANA, LIQSOL, 1));
   BOOST_REQUIRE_EQUAL(STATE_HELD, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());

   BOOST_REQUIRE_EQUAL(success(), set_epoch(2));
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(4 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(6 * UNIT, item_row(1)["remaining"].as_uint64());
   const auto envelope = envelope_row(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(STATE_HELD, envelope["state"].as_string());
   BOOST_REQUIRE_EQUAL(4 * UNIT, envelope["released"].as_uint64());
} FC_LOG_AND_RETHROW()

// A VALID ruling on a challenged, fully bonded request makes the envelope RELEASABLE again: release
// resumes where it stopped. Every bond covered the hold bond, so nothing is forwarded to the challenger
// and nothing is claimed.
BOOST_FIXTURE_TEST_CASE(valid_ruling_resumes_release, sysio_synd_released_tester) try {
   BOOST_REQUIRE_EQUAL(success(), challenge("carol"_n, SOLANA, LIQSOL, 1));
   const int64_t carol = liq_balance("carol"_n);
   BOOST_REQUIRE_EQUAL(success(), rule(id, true));

   BOOST_REQUIRE_EQUAL(success(), set_epoch(2));
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE(!executed(BOND_ACCOUNT, "claim"_n));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(8 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(STATE_RELEASABLE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());

   BOOST_REQUIRE_EQUAL(success(), set_epoch(3));
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(10 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(STATE_DONE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(carol, liq_balance("carol"_n));
   BOOST_REQUIRE_EQUAL(0, liq_balance(SYND_ACCOUNT));
} FC_LOG_AND_RETHROW()

// INVALID after 4 of 10 were released: the 6 still held are burned, the forfeit of 10 is pulled from
// sysio.bond, the 4 released are burned out of it and the other 6 go to the fee pot. Supply is whole:
// what alice kept, the challenger's remainder, the hold bond in sysio.bond and the pot. The hold bond
// is the challenger's to claim.
BOOST_FIXTURE_TEST_CASE(invalid_burns_held_items_and_the_released_part_of_the_forfeit,
                        sysio_synd_released_tester) try {
   BOOST_REQUIRE_EQUAL(success(), challenge("carol"_n, SOLANA, LIQSOL, 1));
   const uint64_t hold_bond = 10 * UNIT * HOLD_BPS / BPS_DENOMINATOR;
   // Minted: the syndication 10, dave's bond 10 and carol's 5.
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(25 * UNIT), supply());
   BOOST_REQUIRE_EQUAL(success(), rule(id, false));

   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE(executed(BOND_ACCOUNT, "claim"_n));
   BOOST_REQUIRE(console_has(QUEUE_INVALID_BURNED));
   BOOST_REQUIRE(item_row(1).is_null());
   const auto envelope = envelope_row(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(STATE_INVALID, envelope["state"].as_string());
   BOOST_REQUIRE_EQUAL(4 * UNIT, envelope["released"].as_uint64());
   BOOST_REQUIRE_EQUAL(10 * UNIT, envelope["burned"].as_uint64());
   BOOST_REQUIRE_EQUAL(0u, bond_request(id)["forfeit_pending"].as_uint64());
   BOOST_REQUIRE_EQUAL(6 * UNIT, feepot_balance(LIQSOL));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(6 * UNIT), liq_balance(SYND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(4 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(15 * UNIT), supply());
   BOOST_REQUIRE_EQUAL(supply(), liq_balance("alice"_n) + liq_balance("carol"_n) + liq_balance(BOND_ACCOUNT) +
                                    liq_balance(SYND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(2u, ledger_row(SOLANA, LIQSOL)["queue_epoch"].as<uint32_t>());

   // The challenger takes the hold bond back from sysio.bond.
   BOOST_REQUIRE_EQUAL(success(), bond_claim(id, "carol"_n));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(4 * UNIT + hold_bond), liq_balance("carol"_n));

   // Finished: the next step touches nothing.
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE(!executed(BOND_ACCOUNT, "claim"_n));
   BOOST_REQUIRE_EQUAL(6 * UNIT, feepot_balance(LIQSOL));
} FC_LOG_AND_RETHROW()

// INVALID before anything was released: the held syndication is burned, the yield report is dropped
// without a mint, and the whole forfeit -- the syndication and the yield it covered -- goes to the fee
// pot, none of it burned.
BOOST_FIXTURE_TEST_CASE(invalid_with_nothing_released_burns_only_held_items_and_pots_the_forfeit,
                        sysio_synd_tester) try {
   const auto pubkey = ed_bytes(ed_key());
   const auto digest = digest_of("envelope-1");
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {}));
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 1, ChainKind::CHAIN_KIND_SVM, pubkey, LIQSOL, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), onyield(SOLANA, 1, digest, 2, 3, LIQSOL, 2 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), closeenv(SOLANA, 1, digest));
   const uint64_t id = envelope_row(SOLANA, LIQSOL, 1)["request_id"].as_uint64();
   underwrite_envelope(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(REQUEST_BONDED, bond_request(id)["state"].as_string());
   BOOST_REQUIRE_EQUAL(success(), liq_mint("carol"_n, LIQSOL, 5 * UNIT));

   // Challenged before any step saw the bond: the envelope is still REQUESTED.
   BOOST_REQUIRE_EQUAL(STATE_REQUESTED, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(success(), challenge("carol"_n, SOLANA, LIQSOL, 1));
   BOOST_REQUIRE_EQUAL(success(), rule(id, false));
   // Minted: the syndication 10, dave's bond 12 and carol's 5.
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(27 * UNIT), supply());

   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE(item_row(1).is_null());
   BOOST_REQUIRE(item_row(2).is_null());
   BOOST_REQUIRE_EQUAL(0, pending());
   const auto envelope = envelope_row(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(STATE_INVALID, envelope["state"].as_string());
   BOOST_REQUIRE_EQUAL(0u, envelope["released"].as_uint64());
   BOOST_REQUIRE_EQUAL(10 * UNIT, envelope["burned"].as_uint64());
   BOOST_REQUIRE_EQUAL(12 * UNIT, feepot_balance(LIQSOL));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(12 * UNIT), liq_balance(SYND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(17 * UNIT), supply());
   BOOST_REQUIRE_EQUAL(supply(), liq_balance("carol"_n) + liq_balance(BOND_ACCOUNT) + liq_balance(SYND_ACCOUNT));
} FC_LOG_AND_RETHROW()

// A request ruled INVALID before anyone bonded it, with no hold and no bounty, owes sysio.synd nothing:
// no claim is sent (it would throw), the held items are burned, and the pair's queue moves on to the
// next envelope, which is requested at once.
BOOST_FIXTURE_TEST_CASE(invalid_unbonded_request_burns_and_moves_on, sysio_synd_tester) try {
   const auto pubkey = ed_bytes(ed_key());
   syndicate_and_close(1, 1, pubkey, LIQSOL, 10 * UNIT);
   syndicate_and_close(2, 2, pubkey, LIQSOL, 4 * UNIT);
   BOOST_REQUIRE_EQUAL(STATE_WAITING, envelope_row(SOLANA, LIQSOL, 2)["state"].as_string());
   BOOST_REQUIRE_EQUAL(success(), rule(1, false));

   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE(!executed(BOND_ACCOUNT, "claim"_n));
   BOOST_REQUIRE(item_row(1).is_null());
   const auto envelope = envelope_row(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(STATE_INVALID, envelope["state"].as_string());
   BOOST_REQUIRE_EQUAL(10 * UNIT, envelope["burned"].as_uint64());
   BOOST_REQUIRE_EQUAL(0u, feepot_balance(LIQSOL));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(4 * UNIT), supply());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(4 * UNIT), liq_balance(SYND_ACCOUNT));

   BOOST_REQUIRE_EQUAL(STATE_REQUESTED, envelope_row(SOLANA, LIQSOL, 2)["state"].as_string());
   BOOST_REQUIRE_EQUAL(2u, envelope_row(SOLANA, LIQSOL, 2)["request_id"].as_uint64());
   BOOST_REQUIRE_EQUAL(2u, ledger_row(SOLANA, LIQSOL)["queue_epoch"].as<uint32_t>());
} FC_LOG_AND_RETHROW()

// The burn of an INVALID envelope spends the step's budget item by item: a step too small to finish it
// keeps the pair waiting, and the forfeit is pulled only by the step that erases the last item.
BOOST_FIXTURE_TEST_CASE(invalid_burn_spans_steps_within_the_budget, sysio_synd_tester) try {
   const auto pubkey = ed_bytes(ed_key());
   const auto digest = digest_of("envelope-1");
   for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
      BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, sequence, ChainKind::CHAIN_KIND_SVM, pubkey, LIQSOL,
                                            2 * UNIT));
   }
   BOOST_REQUIRE_EQUAL(success(), closeenv(SOLANA, 1, digest));
   underwrite_envelope(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(success(), rule(1, false));

   // One unit, for the first item: refreshing the envelope is free.
   BOOST_REQUIRE_EQUAL(success(), crank(1));
   BOOST_REQUIRE(console_has(QUEUE_INVALID_BURNING));
   BOOST_REQUIRE(!executed(BOND_ACCOUNT, "claim"_n));
   BOOST_REQUIRE(item_row(1).is_null());
   BOOST_REQUIRE(!item_row(2).is_null());
   auto envelope = envelope_row(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(STATE_REQUESTED, envelope["state"].as_string());
   BOOST_REQUIRE_EQUAL(2 * UNIT, envelope["burned"].as_uint64());
   // The ruling clears issuance even while the independent release pass still has items to burn.
   BOOST_REQUIRE_EQUAL(2u, ledger_row(SOLANA, LIQSOL)["underwriting_epoch"].as_uint64());

   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE(executed(BOND_ACCOUNT, "claim"_n));
   envelope = envelope_row(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(STATE_INVALID, envelope["state"].as_string());
   BOOST_REQUIRE_EQUAL(6 * UNIT, envelope["burned"].as_uint64());
   BOOST_REQUIRE_EQUAL(6 * UNIT, feepot_balance(LIQSOL));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(6 * UNIT), supply());
} FC_LOG_AND_RETHROW()

// INVALID with no hold returns the bounty to the issuer: sysio.synd claims it with the forfeit and puts
// it back into the fee pot it came from.
BOOST_FIXTURE_TEST_CASE(invalid_without_a_hold_returns_the_bounty_to_the_fee_pot, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.synd_fee_bps = 1000, .bounty = UNIT}));
   syndicate_and_close(1, 1, pubkey, LIQSOL, 20 * UNIT);
   underwrite_envelope(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(2 * UNIT, feepot_balance(LIQSOL));
   syndicate_and_close(2, 2, pubkey, LIQSOL, 10 * UNIT);
   BOOST_REQUIRE_EQUAL(UNIT, bond_request(2)["bounty"].as_uint64());
   BOOST_REQUIRE_EQUAL(UNIT, feepot_balance(LIQSOL));
   BOOST_REQUIRE_EQUAL(success(), rule(2, false));

   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE(executed(BOND_ACCOUNT, "claim"_n));
   BOOST_REQUIRE_EQUAL(STATE_INVALID, envelope_row(SOLANA, LIQSOL, 2)["state"].as_string());
   BOOST_REQUIRE_EQUAL(2 * UNIT, feepot_balance(LIQSOL));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(2 * UNIT), liq_balance(SYND_ACCOUNT));
} FC_LOG_AND_RETHROW()

// VALID after a challenge of a partly bonded request: sysio.bond awards the issuer the share of the hold
// bond no bond covered; sysio.synd claims it and forwards it to the challenger, who paid it, once. The
// envelope then releases.
BOOST_FIXTURE_TEST_CASE(valid_after_challenge_forwards_the_unbonded_hold_share, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {}));
   syndicate_and_close(1, 1, pubkey, LIQSOL, 10 * UNIT);
   const uint64_t id = envelope_row(SOLANA, LIQSOL, 1)["request_id"].as_uint64();
   BOOST_REQUIRE_EQUAL(success(), underwrite("dave"_n, id, 4 * UNIT, LIQSOL));
   BOOST_REQUIRE_EQUAL(success(), liq_mint("carol"_n, LIQSOL, 5 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), challenge("carol"_n, SOLANA, LIQSOL, 1));
   const uint64_t hold_bond = 10 * UNIT * HOLD_BPS / BPS_DENOMINATOR;
   const uint64_t share     = hold_bond * 6 / 10;   // the 6 of 10 no bond covered
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(5 * UNIT - hold_bond), liq_balance("carol"_n));
   BOOST_REQUIRE_EQUAL(success(), rule(id, true));

   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE(executed(BOND_ACCOUNT, "claim"_n));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(5 * UNIT - hold_bond + share), liq_balance("carol"_n));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(10 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(STATE_DONE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(0, liq_balance(SYND_ACCOUNT));

   // Forwarded once; dave takes his bond and his stake's share of the hold bond.
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(5 * UNIT - hold_bond + share), liq_balance("carol"_n));
   BOOST_REQUIRE_EQUAL(success(), bond_claim(id, "dave"_n));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(4 * UNIT + hold_bond - share), liq_balance("dave"_n));
} FC_LOG_AND_RETHROW()

// Anyone may claim sysio.synd's forfeit for it before a step sees the ruling: the forfeit is then already
// in sysio.synd's row, the step sends no claim (it would be refused), and accounts for the forfeit from
// the request's bonded amount all the same.
BOOST_FIXTURE_TEST_CASE(invalid_accounts_for_a_forfeit_claimed_by_someone_else, sysio_synd_released_tester) try {
   BOOST_REQUIRE_EQUAL(success(), challenge("carol"_n, SOLANA, LIQSOL, 1));
   BOOST_REQUIRE_EQUAL(success(), rule(id, false));
   BOOST_REQUIRE_EQUAL(success(), push(BOND_ACCOUNT, bond_abi_ser, "bob"_n, "claim"_n, mvo()
      ("request_id", id)("account", SYND_ACCOUNT)));
   BOOST_REQUIRE_EQUAL(0u, bond_request(id)["forfeit_pending"].as_uint64());

   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE(!executed(BOND_ACCOUNT, "claim"_n));
   const auto envelope = envelope_row(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(STATE_INVALID, envelope["state"].as_string());
   BOOST_REQUIRE_EQUAL(10 * UNIT, envelope["burned"].as_uint64());
   BOOST_REQUIRE_EQUAL(6 * UNIT, feepot_balance(LIQSOL));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(6 * UNIT), liq_balance(SYND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(15 * UNIT), supply());
} FC_LOG_AND_RETHROW()

// Contract challengers receive their refund directly without recipient execution.
BOOST_FIXTURE_TEST_CASE(valid_after_challenge_credits_a_contract_challenger, sysio_synd_tester) try {
   const auto pubkey = ed_bytes(ed_key());
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {}));
   syndicate_and_close(1, 1, pubkey, LIQSOL, 10 * UNIT);
   const uint64_t id = envelope_row(SOLANA, LIQSOL, 1)["request_id"].as_uint64();
   BOOST_REQUIRE_EQUAL(success(), liq_mint("bob"_n, LIQSOL, 5 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), challenge("bob"_n, SOLANA, LIQSOL, 1));
   set_code("bob"_n, contracts::util::block_transfer_wasm());
   produce_blocks();
   BOOST_REQUIRE_EQUAL(success(), rule(id, true));

   // Nothing was bonded: the whole hold bond is the issuer's award.
   BOOST_REQUIRE_EQUAL(success(), crank());
   for (const auto& trace : last_trace->action_traces) BOOST_REQUIRE(trace.receiver != "bob"_n);
   BOOST_REQUIRE(executed(BOND_ACCOUNT, "claim"_n));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(5 * UNIT), liq_balance("bob"_n));
   BOOST_REQUIRE_EQUAL(0u, feepot_balance(LIQSOL));
   BOOST_REQUIRE_EQUAL(STATE_DONE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
} FC_LOG_AND_RETHROW()

// An envelope released whole while its request is still only BONDED can be challenged: the bond exists
// for exactly this. The challenge rewinds the pair's queue to it, and an INVALID ruling burns everything
// it released out of the forfeit.
BOOST_FIXTURE_TEST_CASE(invalid_after_done_challenge_burns_the_released_amount, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {}));
   syndicate_and_close(1, 1, pubkey, LIQSOL, 10 * UNIT);
   const uint64_t id = envelope_row(SOLANA, LIQSOL, 1)["request_id"].as_uint64();
   underwrite_envelope(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(STATE_DONE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(2u, ledger_row(SOLANA, LIQSOL)["queue_epoch"].as<uint32_t>());
   BOOST_REQUIRE_EQUAL(REQUEST_BONDED, bond_request(id)["state"].as_string());

   BOOST_REQUIRE_EQUAL(success(), liq_mint("carol"_n, LIQSOL, 5 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), challenge("carol"_n, SOLANA, LIQSOL, 1));
   BOOST_REQUIRE_EQUAL(STATE_HELD, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(1u, ledger_row(SOLANA, LIQSOL)["queue_epoch"].as<uint32_t>());
   // Minted: the syndication 10, dave's bond 10 and carol's 5.
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(25 * UNIT), supply());
   BOOST_REQUIRE_EQUAL(success(), rule(id, false));

   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE(executed(BOND_ACCOUNT, "claim"_n));
   const auto envelope = envelope_row(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(STATE_INVALID, envelope["state"].as_string());
   BOOST_REQUIRE_EQUAL(10 * UNIT, envelope["burned"].as_uint64());
   BOOST_REQUIRE_EQUAL(0u, feepot_balance(LIQSOL));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(10 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(15 * UNIT), supply());
   BOOST_REQUIRE_EQUAL(supply(), liq_balance("alice"_n) + liq_balance("carol"_n) + liq_balance(BOND_ACCOUNT) +
                                    liq_balance(SYND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(2u, ledger_row(SOLANA, LIQSOL)["queue_epoch"].as<uint32_t>());
} FC_LOG_AND_RETHROW()

// A challenge that would cost nothing -- a hold bond that rounds to zero and no challenge_extra -- is
// refused: it would freeze the pair for free. The pair's extra makes it possible.
BOOST_FIXTURE_TEST_CASE(a_challenge_of_zero_charge_is_refused, sysio_synd_tester) try {
   constexpr std::string_view LIQPT     = "LIQPT";
   const symbol               LIQPT_SYM = symbol::from_string("2,LIQPT");
   BOOST_REQUIRE_EQUAL(success(), regtoken(LIQPT, 2, ChainKind::CHAIN_KIND_SVM, SOLANA,
                                           std::vector<char>(32, char(0x5b))));
   BOOST_REQUIRE_EQUAL(success(), create_shadow(LIQPT_SYM, SOLANA, LIQPT));
   // 5 base units cover a hold bond of 5 * 1000 / 10000 = 0.
   syndicate_and_close(1, 1, ed_bytes(ed_key()), LIQPT, 5);
   BOOST_REQUIRE_EQUAL(STATE_REQUESTED, envelope_row(SOLANA, LIQPT, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(success(), liq_mint("carol"_n, LIQPT, 100));
   BOOST_REQUIRE(mentions(challenge("carol"_n, SOLANA, LIQPT, 1), CHALLENGE_ZERO));

   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQPT, {.challenge_extra = 1}));
   BOOST_REQUIRE_EQUAL(success(), challenge("carol"_n, SOLANA, LIQPT, 1));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(99), liq_balance("carol"_n, LIQPT_SYM));
   BOOST_REQUIRE_EQUAL(1u, feepot_balance(LIQPT));
   BOOST_REQUIRE_EQUAL(STATE_HELD, envelope_row(SOLANA, LIQPT, 1)["state"].as_string());
} FC_LOG_AND_RETHROW()

// A third party may claim sysio.synd's award of the hold bond for it before a step sees the VALID
// ruling: the share is then already in sysio.synd's row, the step sends no claim and still forwards it.
BOOST_FIXTURE_TEST_CASE(valid_forwards_a_hold_share_someone_else_claimed_first, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {}));
   syndicate_and_close(1, 1, pubkey, LIQSOL, 10 * UNIT);
   const uint64_t id = envelope_row(SOLANA, LIQSOL, 1)["request_id"].as_uint64();
   BOOST_REQUIRE_EQUAL(success(), underwrite("dave"_n, id, 4 * UNIT, LIQSOL));
   BOOST_REQUIRE_EQUAL(success(), liq_mint("carol"_n, LIQSOL, 5 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), challenge("carol"_n, SOLANA, LIQSOL, 1));
   const uint64_t hold_bond = 10 * UNIT * HOLD_BPS / BPS_DENOMINATOR;
   const uint64_t share     = hold_bond * 6 / 10;   // the 6 of 10 no bond covered
   BOOST_REQUIRE_EQUAL(success(), rule(id, true));
   BOOST_REQUIRE_EQUAL(success(), push(BOND_ACCOUNT, bond_abi_ser, "bob"_n, "claim"_n, mvo()
      ("request_id", id)("account", SYND_ACCOUNT)));

   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE(!executed(BOND_ACCOUNT, "claim"_n));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(5 * UNIT - hold_bond + share), liq_balance("carol"_n));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(10 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(STATE_DONE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(0, liq_balance(SYND_ACCOUNT));
} FC_LOG_AND_RETHROW()

// INVALID after the envelope's yield report was released: the yield sits in sysio.liq's pending balance,
// outside supply, and is burned out of the forfeit like any release, so supply plus pending is exactly
// what the envelope's holders and the other parties hold.
BOOST_FIXTURE_TEST_CASE(invalid_after_a_yield_release_keeps_the_supply_whole, sysio_synd_tester) try {
   const auto pubkey = ed_bytes(ed_key());
   const auto digest = digest_of("envelope-1");
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.synd_burst = 0, .synd_refill = 0}));
   BOOST_REQUIRE_EQUAL(success(), onyield(SOLANA, 1, digest, 1, 3, LIQSOL, 2 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 2, ChainKind::CHAIN_KIND_SVM, pubkey, LIQSOL, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), closeenv(SOLANA, 1, digest));
   const uint64_t id = envelope_row(SOLANA, LIQSOL, 1)["request_id"].as_uint64();
   underwrite_envelope(SOLANA, LIQSOL, 1);
   // The yield goes out; the syndication waits behind an empty bucket.
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(2 * UNIT), pending());
   BOOST_REQUIRE_EQUAL(2 * UNIT, envelope_row(SOLANA, LIQSOL, 1)["released"].as_uint64());
   BOOST_REQUIRE_EQUAL(success(), liq_mint("carol"_n, LIQSOL, 5 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), challenge("carol"_n, SOLANA, LIQSOL, 1));
   // Minted: the syndication 10, dave's bond 12 and carol's 5; 2 more pending.
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(27 * UNIT), supply());
   BOOST_REQUIRE_EQUAL(success(), rule(id, false));

   BOOST_REQUIRE_EQUAL(success(), crank());
   const auto envelope = envelope_row(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(STATE_INVALID, envelope["state"].as_string());
   // The held 10 and the released 2 out of the forfeit of 12.
   BOOST_REQUIRE_EQUAL(12 * UNIT, envelope["burned"].as_uint64());
   BOOST_REQUIRE_EQUAL(10 * UNIT, feepot_balance(LIQSOL));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(15 * UNIT), supply());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(2 * UNIT), pending());
   BOOST_REQUIRE_EQUAL(supply(), liq_balance("carol"_n) + liq_balance(BOND_ACCOUNT) + liq_balance(SYND_ACCOUNT));
} FC_LOG_AND_RETHROW()

// The forfeit and the bounty returned are snapshotted the first time a step sees INVALID. With the burn
// spread over steps, a third party may claim both for sysio.synd and sysio.bond may prune the request in
// between: the step that completes the burn sends no claim and pots them from the snapshot.
BOOST_FIXTURE_TEST_CASE(invalid_completes_from_the_snapshot_after_a_claim_and_a_prune, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   const auto digest = digest_of("envelope-2");
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.synd_fee_bps = 1000, .bounty = UNIT}));
   // A first envelope fills the fee pot with 2 of fees.
   syndicate_and_close(1, 1, pubkey, LIQSOL, 20 * UNIT);
   underwrite_envelope(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(2 * UNIT, feepot_balance(LIQSOL));
   // The second envelope, three items, takes a bounty of 1 from the pot.
   for (uint64_t sequence = 2; sequence <= 4; ++sequence) {
      BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 2, digest, sequence, ChainKind::CHAIN_KIND_SVM, pubkey, LIQSOL,
                                            2 * UNIT));
   }
   BOOST_REQUIRE_EQUAL(success(), closeenv(SOLANA, 2, digest));
   const uint64_t id = envelope_row(SOLANA, LIQSOL, 2)["request_id"].as_uint64();
   BOOST_REQUIRE_EQUAL(UNIT, bond_request(id)["bounty"].as_uint64());
   BOOST_REQUIRE_EQUAL(UNIT, feepot_balance(LIQSOL));
   underwrite_envelope(SOLANA, LIQSOL, 2);
   BOOST_REQUIRE_EQUAL(success(), rule(id, false));

   // One step sees INVALID and burns one item: the snapshot is taken.
   BOOST_REQUIRE_EQUAL(success(), crank(1));
   BOOST_REQUIRE(console_has(QUEUE_INVALID_BURNING));
   auto envelope = envelope_row(SOLANA, LIQSOL, 2);
   BOOST_REQUIRE_EQUAL("INVALID", envelope["outcome"].as_string());
   BOOST_REQUIRE_EQUAL(6 * UNIT, envelope["forfeit"].as_uint64());
   BOOST_REQUIRE_EQUAL(UNIT, envelope["bounty_returned"].as_uint64());

   // Bob claims the forfeit and the bounty for sysio.synd; a week later the request is pruned.
   BOOST_REQUIRE_EQUAL(success(), push(BOND_ACCOUNT, bond_abi_ser, "bob"_n, "claim"_n, mvo()
      ("request_id", id)("account", SYND_ACCOUNT)));
   produce_block(fc::seconds(PRUNE_RETENTION_SEC));
   BOOST_REQUIRE_EQUAL(success(), bond_prune(id, 1));
   BOOST_REQUIRE(bond_request(id).is_null());

   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE(!executed(BOND_ACCOUNT, "claim"_n));
   envelope = envelope_row(SOLANA, LIQSOL, 2);
   BOOST_REQUIRE_EQUAL(STATE_INVALID, envelope["state"].as_string());
   BOOST_REQUIRE_EQUAL(6 * UNIT, envelope["burned"].as_uint64());
   // The pot: 1 left, the forfeit of 6 and the bounty of 1 back.
   BOOST_REQUIRE_EQUAL(8 * UNIT, feepot_balance(LIQSOL));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(8 * UNIT), liq_balance(SYND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(3u, ledger_row(SOLANA, LIQSOL)["queue_epoch"].as<uint32_t>());
} FC_LOG_AND_RETHROW()

// INVALID after part of the envelope was released into `parked` (its pubkey unlinked): the parked row
// keeps what it received, like any released holder, and the part released is burned out of the forfeit.
BOOST_FIXTURE_TEST_CASE(invalid_after_a_partial_park_burns_the_parked_part_out_of_the_forfeit,
                        sysio_synd_tester) try {
   const auto pubkey = ed_bytes(ed_key());
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.synd_burst = 4 * UNIT, .synd_refill = 4 * UNIT}));
   BOOST_REQUIRE_EQUAL(success(), set_epoch(1));
   syndicate_and_close(1, 1, pubkey, LIQSOL, 10 * UNIT);
   const uint64_t id = envelope_row(SOLANA, LIQSOL, 1)["request_id"].as_uint64();
   underwrite_envelope(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(4 * UNIT, parked_balance(LIQSOL, ChainKind::CHAIN_KIND_SVM, pubkey));
   BOOST_REQUIRE_EQUAL(success(), liq_mint("carol"_n, LIQSOL, 5 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), challenge("carol"_n, SOLANA, LIQSOL, 1));
   BOOST_REQUIRE_EQUAL(success(), rule(id, false));

   BOOST_REQUIRE_EQUAL(success(), crank());
   const auto envelope = envelope_row(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(STATE_INVALID, envelope["state"].as_string());
   BOOST_REQUIRE_EQUAL(10 * UNIT, envelope["burned"].as_uint64());
   BOOST_REQUIRE_EQUAL(6 * UNIT, feepot_balance(LIQSOL));
   BOOST_REQUIRE_EQUAL(4 * UNIT, parked_balance(LIQSOL, ChainKind::CHAIN_KIND_SVM, pubkey));
   // sysio.synd's row holds the parked 4 and the pot's 6.
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(10 * UNIT), liq_balance(SYND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(15 * UNIT), supply());
   BOOST_REQUIRE_EQUAL(supply(), liq_balance("carol"_n) + liq_balance(BOND_ACCOUNT) + liq_balance(SYND_ACCOUNT));
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// The emergency stop (sysio.andon)
// ---------------------------------------------------------------------------

// Review focus 3: a crank while the cord is pulled releases nothing and spends nothing of the bucket; after
// the clear the same item releases, and the bucket holds its level at the pull plus the refill of the
// epochs after the clear only. Burst 20, refill 4 per epoch: drained at epoch 1, pulled at 3, cleared at 5,
// cranked at 7 -- epoch 2 refills 4 before the pull, epochs 3 to 5 are frozen, 6 and 7 refill 8: 12 pass,
// where an unfrozen bucket would pass its whole burst of 20.
BOOST_FIXTURE_TEST_CASE(a_freeze_releases_nothing_and_adds_no_bucket_capacity, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.synd_burst = 20 * UNIT, .synd_refill = 4 * UNIT}));
   BOOST_REQUIRE_EQUAL(success(), set_epoch(1));
   syndicate_and_close(1, 1, pubkey, LIQSOL, 60 * UNIT);
   underwrite_envelope(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(20 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(0u, synd_bucket_row(SOLANA, LIQSOL)["level"].as_uint64());

   BOOST_REQUIRE_EQUAL(success(), set_epoch(3));
   BOOST_REQUIRE_EQUAL(success(), andon_pull());

   // Frozen: the crank runs, prints why it waits, and moves nothing -- not the item, not the bucket.
   BOOST_REQUIRE_EQUAL(success(), set_epoch(4));
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE(console_has(QUEUE_FROZEN));
   BOOST_REQUIRE(!executed(LIQ_ACCOUNT, "transfer"_n));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(20 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(40 * UNIT, item_row(1)["remaining"].as_uint64());
   auto envelope = envelope_row(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(STATE_RELEASABLE, envelope["state"].as_string());
   BOOST_REQUIRE_EQUAL(20 * UNIT, envelope["released"].as_uint64());
   auto bucket = synd_bucket_row(SOLANA, LIQSOL);
   BOOST_REQUIRE_EQUAL(0u, bucket["level"].as_uint64());
   BOOST_REQUIRE_EQUAL(1u, bucket["last_epoch"].as<uint32_t>());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(40 * UNIT), liq_balance(SYND_ACCOUNT));

   BOOST_REQUIRE_EQUAL(success(), set_epoch(5));
   BOOST_REQUIRE_EQUAL(success(), andon_clear());
   BOOST_REQUIRE_EQUAL(success(), set_epoch(7));
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE(!console_has(QUEUE_FROZEN));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(32 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(28 * UNIT, item_row(1)["remaining"].as_uint64());
   bucket = synd_bucket_row(SOLANA, LIQSOL);
   BOOST_REQUIRE_EQUAL(0u, bucket["level"].as_uint64());
   BOOST_REQUIRE_EQUAL(7u, bucket["last_epoch"].as<uint32_t>());
   BOOST_REQUIRE_EQUAL(3u, bucket["frozen_mark"].as_uint64());   // epochs 3, 4 and 5

   // From here the bucket refills as before: one epoch, 4 more.
   BOOST_REQUIRE_EQUAL(success(), set_epoch(8));
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(36 * UNIT), liq_balance("alice"_n));
} FC_LOG_AND_RETHROW()

// Review focus 2, sysio.synd: while the cord is pulled a signed transfer INTO sysio.synd succeeds -- a
// challenge pays its charge in and holds the request -- and everything that would move funds out or burn
// them is refused: desyndicate, sweep, sweepyield and dropenv. Intake still holds new syndications. After
// the clear each refused action succeeds.
BOOST_FIXTURE_TEST_CASE(a_freeze_admits_what_comes_in_and_refuses_what_leaves, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   constexpr auto SVM = ChainKind::CHAIN_KIND_SVM;
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.challenge_extra = UNIT}));
   BOOST_REQUIRE_EQUAL(success(), liq_mint("alice"_n, LIQSOL, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), liq_mint("carol"_n, LIQSOL, 5 * UNIT));
   syndicate_and_close(1, 1, pubkey, LIQSOL, 10 * UNIT);
   const uint64_t id = envelope_row(SOLANA, LIQSOL, 1)["request_id"].as_uint64();
   BOOST_REQUIRE_EQUAL(REQUEST_OPEN, bond_request(id)["state"].as_string());

   BOOST_REQUIRE_EQUAL(success(), andon_pull());

   // In: the challenger's charge moves into sysio.synd and the hold bond on into sysio.bond.
   const uint64_t hold_bond = 10 * UNIT * HOLD_BPS / BPS_DENOMINATOR;
   BOOST_REQUIRE_EQUAL(success(), challenge("carol"_n, SOLANA, LIQSOL, 1));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(5 * UNIT - hold_bond - UNIT), liq_balance("carol"_n));
   BOOST_REQUIRE_EQUAL(STATE_HELD, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(REQUEST_HELD, bond_request(id)["state"].as_string());
   BOOST_REQUIRE_EQUAL(UNIT, feepot_balance(LIQSOL));

   // In: intake of a new envelope, left OPEN.
   const auto digest2 = digest_of("envelope-2");
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 2, digest2, 2, SVM, pubkey, LIQSOL, 3 * UNIT));
   BOOST_REQUIRE_EQUAL(3 * UNIT, item_row(2)["amount"].as_uint64());

   // Out: refused, and nothing moved.
   BOOST_REQUIRE_EQUAL(frozen(), desyndicate("alice"_n, 4 * UNIT));
   BOOST_REQUIRE_EQUAL(frozen(), sweep("alice"_n, SVM));
   BOOST_REQUIRE_EQUAL(frozen(), push(SYND_ACCOUNT, synd_abi_ser, "alice"_n, "sweepyield"_n,
                                      mvo()("token_code", codename_mvo(LIQSOL))));
   BOOST_REQUIRE_EQUAL(frozen(), dropenv(SOLANA, LIQSOL, 2));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(10 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(STATE_OPEN, envelope_row(SOLANA, LIQSOL, 2)["state"].as_string());

   BOOST_REQUIRE_EQUAL(success(), andon_clear());
   BOOST_REQUIRE_EQUAL(success(), desyndicate("alice"_n, 4 * UNIT));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(6 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(success(), sweep("alice"_n, SVM));
   BOOST_REQUIRE_EQUAL(success(), push(SYND_ACCOUNT, synd_abi_ser, "alice"_n, "sweepyield"_n,
                                       mvo()("token_code", codename_mvo(LIQSOL))));
   BOOST_REQUIRE_EQUAL(success(), dropenv(SOLANA, LIQSOL, 2));
   BOOST_REQUIRE_EQUAL(STATE_INVALID, envelope_row(SOLANA, LIQSOL, 2)["state"].as_string());
} FC_LOG_AND_RETHROW()

// A delivery from `parked` waits for the clear: a link made while the cord is pulled stands, `linkswept`
// delivers nothing and says so, and `sweep` after the clear delivers the balance and the WIRE it banked.
BOOST_FIXTURE_TEST_CASE(a_freeze_defers_the_parked_delivery_of_a_new_link, sysio_synd_tester) try {
   const auto priv   = fc::crypto::private_key::generate(fc::crypto::private_key::key_type::em);
   const auto pubkey = em_bytes(priv.get_public_key());
   BOOST_REQUIRE_EQUAL(success(), importsynd(ETH, LIQETH, { credit(pubkey, 30 * UNIT) }));
   BOOST_REQUIRE_EQUAL(success(), addyield("carol"_n, 3 * UNIT, LIQETH_SYM));

   BOOST_REQUIRE_EQUAL(success(), andon_pull());
   BOOST_REQUIRE_EQUAL(success(), createlink("alice"_n, priv));
   BOOST_REQUIRE(executed(SYND_ACCOUNT, "linkswept"_n));
   BOOST_REQUIRE(console_has(LINKSWEPT_FROZEN));
   BOOST_REQUIRE_EQUAL(30 * UNIT, parked_balance(LIQETH, ChainKind::CHAIN_KIND_EVM, pubkey));
   BOOST_REQUIRE_EQUAL(0, liq_balance("alice"_n, LIQETH_SYM));
   BOOST_REQUIRE_EQUAL(frozen(), sweep("alice"_n, ChainKind::CHAIN_KIND_EVM));

   BOOST_REQUIRE_EQUAL(success(), andon_clear());
   BOOST_REQUIRE_EQUAL(success(), sweep("alice"_n, ChainKind::CHAIN_KIND_EVM));
   BOOST_REQUIRE(parked_row(LIQETH, ChainKind::CHAIN_KIND_EVM, pubkey).is_null());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(30 * UNIT), liq_balance("alice"_n, LIQETH_SYM));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(3 * UNIT), liq_owed("alice"_n, LIQETH_SYM));
} FC_LOG_AND_RETHROW()

// An INVALID ruling seen while the cord is pulled is recorded with its snapshot, but nothing is burned and
// nothing claimed from sysio.bond: the pair waits. The first step after the clear burns exactly what the
// unfrozen step would have: the 6 held, then the 4 released out of the forfeit, the rest to the fee pot.
BOOST_FIXTURE_TEST_CASE(a_freeze_defers_the_invalid_burn, sysio_synd_released_tester) try {
   BOOST_REQUIRE_EQUAL(success(), challenge("carol"_n, SOLANA, LIQSOL, 1));
   BOOST_REQUIRE_EQUAL(success(), rule(id, false));
   BOOST_REQUIRE_EQUAL(success(), andon_pull());

   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE(console_has(QUEUE_FROZEN_BURN));
   BOOST_REQUIRE(!executed(BOND_ACCOUNT, "claim"_n));
   BOOST_REQUIRE(!executed(LIQ_ACCOUNT, "burn"_n));
   auto envelope = envelope_row(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(STATE_HELD, envelope["state"].as_string());
   BOOST_REQUIRE_EQUAL("INVALID", envelope["outcome"].as_string());
   BOOST_REQUIRE_EQUAL(10 * UNIT, envelope["forfeit"].as_uint64());
   BOOST_REQUIRE_EQUAL(0u, envelope["burned"].as_uint64());
   BOOST_REQUIRE_EQUAL(6 * UNIT, item_row(1)["remaining"].as_uint64());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(25 * UNIT), supply());

   BOOST_REQUIRE_EQUAL(success(), andon_clear());
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE(executed(BOND_ACCOUNT, "claim"_n));
   BOOST_REQUIRE(console_has(QUEUE_INVALID_BURNED));
   envelope = envelope_row(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(STATE_INVALID, envelope["state"].as_string());
   BOOST_REQUIRE_EQUAL(10 * UNIT, envelope["burned"].as_uint64());
   BOOST_REQUIRE(item_row(1).is_null());
   BOOST_REQUIRE_EQUAL(6 * UNIT, feepot_balance(LIQSOL));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(15 * UNIT), supply());
} FC_LOG_AND_RETHROW()

// A VALID ruling seen while the cord is pulled is recorded with the challenger's hold share snapshotted and
// pending, but nothing is claimed or forwarded; the first step after the clear forwards the share once and
// releases the envelope.
BOOST_FIXTURE_TEST_CASE(a_freeze_defers_the_hold_share_of_a_valid_ruling, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {}));
   syndicate_and_close(1, 1, pubkey, LIQSOL, 10 * UNIT);
   const uint64_t id = envelope_row(SOLANA, LIQSOL, 1)["request_id"].as_uint64();
   BOOST_REQUIRE_EQUAL(success(), underwrite("dave"_n, id, 4 * UNIT, LIQSOL));
   BOOST_REQUIRE_EQUAL(success(), liq_mint("carol"_n, LIQSOL, 5 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), challenge("carol"_n, SOLANA, LIQSOL, 1));
   const uint64_t hold_bond = 10 * UNIT * HOLD_BPS / BPS_DENOMINATOR;
   const uint64_t share     = hold_bond * 6 / 10;   // the 6 of 10 no bond covered
   BOOST_REQUIRE_EQUAL(success(), rule(id, true));
   BOOST_REQUIRE_EQUAL(success(), andon_pull());

   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE(console_has(QUEUE_FROZEN_SHARE));
   BOOST_REQUIRE(!executed(BOND_ACCOUNT, "claim"_n));
   auto envelope = envelope_row(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL("VALID", envelope["outcome"].as_string());
   BOOST_REQUIRE_EQUAL(STATE_RELEASABLE, envelope["state"].as_string());
   BOOST_REQUIRE_EQUAL(share, envelope["hold_share"].as_uint64());
   BOOST_REQUIRE_EQUAL("carol"_n, envelope["hold_beneficiary"].as<name>());
   BOOST_REQUIRE(envelope["share_pending"].as_bool());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(5 * UNIT - hold_bond), liq_balance("carol"_n));
   BOOST_REQUIRE_EQUAL(0, liq_balance("alice"_n));

   BOOST_REQUIRE_EQUAL(success(), andon_clear());
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(5 * UNIT - hold_bond + share), liq_balance("carol"_n));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(10 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(STATE_DONE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE(!envelope_row(SOLANA, LIQSOL, 1)["share_pending"].as_bool());
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(5 * UNIT - hold_bond + share), liq_balance("carol"_n));
} FC_LOG_AND_RETHROW()

// A request is still issued while the cord is pulled: closeenv requests the next envelope and a bond can be
// placed on it -- the bond moves INTO sysio.bond -- while nothing releases until the clear.
BOOST_FIXTURE_TEST_CASE(a_freeze_still_issues_requests_and_takes_bonds, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {}));
   BOOST_REQUIRE_EQUAL(success(), andon_pull());

   syndicate_and_close(1, 1, pubkey, LIQSOL, 10 * UNIT);
   BOOST_REQUIRE(console_has(QUEUE_FROZEN));
   BOOST_REQUIRE_EQUAL(STATE_REQUESTED, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   underwrite_envelope(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(STATE_RELEASABLE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(0, liq_balance("alice"_n));

   BOOST_REQUIRE_EQUAL(success(), andon_clear());
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(10 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(STATE_DONE, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
} FC_LOG_AND_RETHROW()

// The forward survives a prune: every claim on a VALID request is made before the pull (the underwriter's,
// and sysio.synd's award claimed for it by a bystander), the frozen step records VALID with its snapshot,
// and sysio.bond prunes the request past its retention while the cord is still pulled. The first step after
// the clear forwards the challenger's share from the snapshot, once, and releases the envelope.
BOOST_FIXTURE_TEST_CASE(a_valid_forward_survives_a_prune_during_the_freeze, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {}));
   syndicate_and_close(1, 1, pubkey, LIQSOL, 10 * UNIT);
   const uint64_t id = envelope_row(SOLANA, LIQSOL, 1)["request_id"].as_uint64();
   BOOST_REQUIRE_EQUAL(success(), underwrite("dave"_n, id, 4 * UNIT, LIQSOL));
   BOOST_REQUIRE_EQUAL(success(), liq_mint("carol"_n, LIQSOL, 5 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), challenge("carol"_n, SOLANA, LIQSOL, 1));
   const uint64_t hold_bond = 10 * UNIT * HOLD_BPS / BPS_DENOMINATOR;
   const uint64_t share     = hold_bond * 6 / 10;   // the 6 of 10 no bond covered
   BOOST_REQUIRE_EQUAL(success(), rule(id, true));
   BOOST_REQUIRE_EQUAL(success(), bond_claim(id, "dave"_n));
   BOOST_REQUIRE_EQUAL(success(), push(BOND_ACCOUNT, bond_abi_ser, "bob"_n, "claim"_n, mvo()
      ("request_id", id)("account", SYND_ACCOUNT)));

   BOOST_REQUIRE_EQUAL(success(), andon_pull());
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL("VALID", envelope_row(SOLANA, LIQSOL, 1)["outcome"].as_string());
   BOOST_REQUIRE(envelope_row(SOLANA, LIQSOL, 1)["share_pending"].as_bool());

   produce_block(fc::seconds(PRUNE_RETENTION_SEC + 1));
   BOOST_REQUIRE_EQUAL(success(), bond_prune(id, 10));
   BOOST_REQUIRE(bond_request(id).is_null());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(5 * UNIT - hold_bond), liq_balance("carol"_n));

   BOOST_REQUIRE_EQUAL(success(), andon_clear());
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE(!executed(BOND_ACCOUNT, "claim"_n));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(5 * UNIT - hold_bond + share), liq_balance("carol"_n));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(10 * UNIT), liq_balance("alice"_n));
   const auto envelope = envelope_row(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(STATE_DONE, envelope["state"].as_string());
   BOOST_REQUIRE(!envelope["share_pending"].as_bool());
   BOOST_REQUIRE_EQUAL(0, liq_balance(SYND_ACCOUNT));
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(5 * UNIT - hold_bond + share), liq_balance("carol"_n));
} FC_LOG_AND_RETHROW()

// A bucket holding a positive level at the pull keeps it through a frozen crank -- nothing ticks and
// nothing is spent -- and after the clear it has that level plus the refill of the epochs after the clear.
// Burst 40, refill 2: a full bucket passes 10 at epoch 1 (30 left); the pull at 3 finds 32 (epoch 2's
// refill); epochs 3 to 5 are frozen; at 7 it holds 36, and the 30 syndicated during the freeze pass
// whole, leaving 6 (an unfrozen bucket would hold its burst of 40 and leave 10).
BOOST_FIXTURE_TEST_CASE(a_freeze_keeps_a_positive_bucket_level, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.synd_burst = 40 * UNIT, .synd_refill = 2 * UNIT}));
   BOOST_REQUIRE_EQUAL(success(), set_epoch(1));
   syndicate_and_close(1, 1, pubkey, LIQSOL, 10 * UNIT);
   underwrite_envelope(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(10 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(30 * UNIT, synd_bucket_row(SOLANA, LIQSOL)["level"].as_uint64());

   BOOST_REQUIRE_EQUAL(success(), set_epoch(3));
   BOOST_REQUIRE_EQUAL(success(), andon_pull());
   syndicate_and_close(3, 2, pubkey, LIQSOL, 30 * UNIT);
   underwrite_envelope(SOLANA, LIQSOL, 3);
   BOOST_REQUIRE_EQUAL(success(), set_epoch(4));
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE(console_has(QUEUE_FROZEN));
   BOOST_REQUIRE_EQUAL(STATE_RELEASABLE, envelope_row(SOLANA, LIQSOL, 3)["state"].as_string());
   auto bucket = synd_bucket_row(SOLANA, LIQSOL);
   BOOST_REQUIRE_EQUAL(30 * UNIT, bucket["level"].as_uint64());
   BOOST_REQUIRE_EQUAL(1u, bucket["last_epoch"].as<uint32_t>());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(10 * UNIT), liq_balance("alice"_n));

   BOOST_REQUIRE_EQUAL(success(), set_epoch(5));
   BOOST_REQUIRE_EQUAL(success(), andon_clear());
   BOOST_REQUIRE_EQUAL(success(), set_epoch(7));
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(40 * UNIT), liq_balance("alice"_n));
   BOOST_REQUIRE_EQUAL(STATE_DONE, envelope_row(SOLANA, LIQSOL, 3)["state"].as_string());
   bucket = synd_bucket_row(SOLANA, LIQSOL);
   BOOST_REQUIRE_EQUAL(6 * UNIT, bucket["level"].as_uint64());
   BOOST_REQUIRE_EQUAL(7u, bucket["last_epoch"].as<uint32_t>());
} FC_LOG_AND_RETHROW()

// Two freezes between two ticks of a bucket, one of them pulled, cleared and pulled again in the same
// epoch: each frozen epoch is counted once. Burst 20, refill 4: drained at epoch 1; frozen 2 (pulled,
// cleared, re-pulled), cleared at 3; frozen again 5 to 6; ticked at 8. Epochs 2, 3, 5 and 6 are frozen,
// so 4, 7 and 8 refill 12 (a count of epoch 2 twice would leave 8).
BOOST_FIXTURE_TEST_CASE(two_freezes_between_two_ticks_count_each_epoch_once, sysio_synd_tester) try {
   const auto key    = ed_key();
   const auto pubkey = ed_bytes(key);
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, key));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.synd_burst = 20 * UNIT, .synd_refill = 4 * UNIT}));
   BOOST_REQUIRE_EQUAL(success(), set_epoch(1));
   syndicate_and_close(1, 1, pubkey, LIQSOL, 60 * UNIT);
   underwrite_envelope(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(0u, synd_bucket_row(SOLANA, LIQSOL)["level"].as_uint64());

   BOOST_REQUIRE_EQUAL(success(), set_epoch(2));
   BOOST_REQUIRE_EQUAL(success(), andon_pull());
   BOOST_REQUIRE_EQUAL(success(), andon_clear());
   BOOST_REQUIRE_EQUAL(success(), andon_pull());
   BOOST_REQUIRE_EQUAL(success(), set_epoch(3));
   BOOST_REQUIRE_EQUAL(success(), andon_clear());
   BOOST_REQUIRE_EQUAL(success(), set_epoch(5));
   BOOST_REQUIRE_EQUAL(success(), andon_pull());
   BOOST_REQUIRE_EQUAL(success(), set_epoch(6));
   BOOST_REQUIRE_EQUAL(success(), andon_clear());

   BOOST_REQUIRE_EQUAL(success(), set_epoch(8));
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(32 * UNIT), liq_balance("alice"_n));
   const auto bucket = synd_bucket_row(SOLANA, LIQSOL);
   BOOST_REQUIRE_EQUAL(0u, bucket["level"].as_uint64());
   BOOST_REQUIRE_EQUAL(4u, bucket["frozen_mark"].as_uint64());
} FC_LOG_AND_RETHROW()

// ---------------------------------------------------------------------------
// The solvency check: outpost custody >= depot outstanding
// ---------------------------------------------------------------------------

// Custody equal to the outstanding: nothing is recorded or printed, the cord stays clear. A syndication is
// compared after its own mint, a yield report with the outstanding as it stands.
BOOST_FIXTURE_TEST_CASE(custody_equal_to_the_outstanding_records_nothing, sysio_synd_tester) try {
   const auto     pubkey = ed_bytes(ed_key());
   constexpr auto SVM    = ChainKind::CHAIN_KIND_SVM;
   const auto     digest = digest_of("envelope-1");
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 1, SVM, pubkey, LIQSOL, 10 * UNIT, MSGCH_ACCOUNT,
                                         10 * UNIT));
   BOOST_REQUIRE(!console_has(EXCESS));
   require_no_shortfall();
   BOOST_REQUIRE_EQUAL(success(), onyield(SOLANA, 1, digest, 2, 7, LIQSOL, 2 * UNIT, MSGCH_ACCOUNT, 10 * UNIT));
   BOOST_REQUIRE(!console_has(EXCESS));
   require_no_shortfall();
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(10 * UNIT), supply());
} FC_LOG_AND_RETHROW()

// Custody above the outstanding -- a donation to the custody account -- is normal: it is printed as EXCESS
// and nothing else happens.
BOOST_FIXTURE_TEST_CASE(an_excess_is_printed_and_nothing_else, sysio_synd_tester) try {
   const auto     pubkey = ed_bytes(ed_key());
   constexpr auto SVM    = ChainKind::CHAIN_KIND_SVM;
   const auto     digest = digest_of("envelope-1");
   // 10 locked, and 3 donated to the pool account before the outpost read it.
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 1, SVM, pubkey, LIQSOL, 10 * UNIT, MSGCH_ACCOUNT,
                                         13 * UNIT));
   BOOST_REQUIRE(console_has(verdict(ONSYND_PATH, EXCESS, 13 * UNIT, 10 * UNIT)));
   require_no_shortfall();
   BOOST_REQUIRE_EQUAL(success(), onyield(SOLANA, 1, digest, 2, 7, LIQSOL, 2 * UNIT, MSGCH_ACCOUNT, 15 * UNIT));
   BOOST_REQUIRE(console_has(verdict(ONYIELD_PATH, EXCESS, 15 * UNIT, 10 * UNIT)));
   require_no_shortfall();
} FC_LOG_AND_RETHROW()

// A syndication whose carried custody is below the outstanding after its own mint writes one `mismatch` row
// and pulls the cord, and the syndication is still held, minted and added to its envelope and the ledger.
BOOST_FIXTURE_TEST_CASE(a_syndication_shortfall_records_pulls_and_still_holds, sysio_synd_tester) try {
   const auto     pubkey = ed_bytes(ed_key());
   constexpr auto SVM    = ChainKind::CHAIN_KIND_SVM;
   const auto     digest = digest_of("envelope-1");
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 1, SVM, pubkey, LIQSOL, 10 * UNIT, MSGCH_ACCOUNT,
                                         10 * UNIT));
   require_no_shortfall();

   // 5 more locked, but the outpost holds 12 against 15 outstanding after this mint.
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 2, SVM, pubkey, LIQSOL, 5 * UNIT, MSGCH_ACCOUNT,
                                         12 * UNIT));
   BOOST_REQUIRE(console_has(verdict(ONSYND_PATH, SHORTFALL, 12 * UNIT, 15 * UNIT)));
   BOOST_REQUIRE(console_has(pulled_line(ONSYND_PATH, SOLANA, LIQSOL, 2)));
   const auto rows = mismatch_rows();
   BOOST_REQUIRE_EQUAL(1u, rows.size());
   require_mismatch(rows[0], SOLANA, LIQSOL, 1, 2, MISMATCH_SYNDICATION, 12 * UNIT, 15 * UNIT);

   BOOST_REQUIRE(executed(sysio_system::test_support::andon::account, "pull"_n));
   const auto cord = cord_row();
   BOOST_REQUIRE(cord["pulled"].as_bool());
   BOOST_REQUIRE_EQUAL(SYND_ACCOUNT, cord["pulled_by"].as<name>());
   BOOST_REQUIRE_EQUAL(pull_reason(SOLANA, LIQSOL, 2), cord["reason"].as_string());
   BOOST_REQUIRE_EQUAL(1u, cord["pull_count"].as_uint64());

   // Held and minted as usual.
   BOOST_REQUIRE(executed(LIQ_ACCOUNT, "mint"_n));
   BOOST_REQUIRE_EQUAL(5 * UNIT, item_row(2)["amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(15 * UNIT), supply());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(15 * UNIT), liq_balance(SYND_ACCOUNT));
   const auto envelope = envelope_row(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(15 * UNIT, envelope["synd_total"].as_uint64());
   BOOST_REQUIRE_EQUAL(2u, envelope["item_count"].as<uint32_t>());
   BOOST_REQUIRE_EQUAL(2u, cursor_row(SOLANA)["last_sequence"].as_uint64());

   // The envelope closes as usual; the frozen queue step waits.
   BOOST_REQUIRE_EQUAL(success(), closeenv(SOLANA, 1, digest));
   BOOST_REQUIRE(console_has(QUEUE_FROZEN));
   BOOST_REQUIRE_NE(STATE_OPEN, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
} FC_LOG_AND_RETHROW()

// A yield report whose carried custody is below the outstanding is a shortfall the same way; the report is
// still held as a YIELD item and mints nothing.
BOOST_FIXTURE_TEST_CASE(a_yield_shortfall_records_pulls_and_still_holds, sysio_synd_tester) try {
   const auto     pubkey = ed_bytes(ed_key());
   constexpr auto SVM    = ChainKind::CHAIN_KIND_SVM;
   const auto     digest = digest_of("envelope-1");
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 1, SVM, pubkey, LIQSOL, 10 * UNIT, MSGCH_ACCOUNT,
                                         10 * UNIT));
   // 3 of yield claimed, but the outpost holds 9 against 10 outstanding.
   BOOST_REQUIRE_EQUAL(success(), onyield(SOLANA, 1, digest, 2, 7, LIQSOL, 3 * UNIT, MSGCH_ACCOUNT, 9 * UNIT));
   BOOST_REQUIRE(console_has(verdict(ONYIELD_PATH, SHORTFALL, 9 * UNIT, 10 * UNIT)));
   BOOST_REQUIRE(console_has(pulled_line(ONYIELD_PATH, SOLANA, LIQSOL, 2)));
   const auto rows = mismatch_rows();
   BOOST_REQUIRE_EQUAL(1u, rows.size());
   require_mismatch(rows[0], SOLANA, LIQSOL, 1, 2, MISMATCH_YIELD, 9 * UNIT, 10 * UNIT);
   BOOST_REQUIRE(cord_pulled());
   BOOST_REQUIRE_EQUAL(pull_reason(SOLANA, LIQSOL, 2), cord_row()["reason"].as_string());

   BOOST_REQUIRE_EQUAL(KIND_YIELD, item_row(2)["kind"].as_string());
   BOOST_REQUIRE_EQUAL(3 * UNIT, item_row(2)["remaining"].as_uint64());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(10 * UNIT), supply());
   BOOST_REQUIRE_EQUAL(0, pending());
   BOOST_REQUIRE_EQUAL(3 * UNIT, envelope_row(SOLANA, LIQSOL, 1)["yield_total"].as_uint64());
   BOOST_REQUIRE_EQUAL(7u, cursor_row(SOLANA)["last_epoch"].as_uint64());
} FC_LOG_AND_RETHROW()

// Review focus 3: two syndications of one envelope each carry the custody the outpost read after locking
// it, and each is compared with the outstanding after its own mint -- so both are exact, and so is the
// yield report after them, which carries the custody after its claim.
BOOST_FIXTURE_TEST_CASE(each_syndication_of_an_envelope_passes_on_its_own_custody, sysio_synd_tester) try {
   const auto     pubkey = ed_bytes(ed_key());
   constexpr auto SVM    = ChainKind::CHAIN_KIND_SVM;
   const auto     digest = digest_of("envelope-1");
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 1, SVM, pubkey, LIQSOL, 10 * UNIT, MSGCH_ACCOUNT,
                                         10 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 2, SVM, pubkey, LIQSOL, 20 * UNIT, MSGCH_ACCOUNT,
                                         30 * UNIT));
   BOOST_REQUIRE(!console_has(EXCESS));
   // Yield of 4 claimed into custody: 34 held against 30 outstanding, the report minting nothing yet.
   BOOST_REQUIRE_EQUAL(success(), onyield(SOLANA, 1, digest, 3, 7, LIQSOL, 4 * UNIT, MSGCH_ACCOUNT, 34 * UNIT));
   BOOST_REQUIRE(console_has(verdict(ONYIELD_PATH, EXCESS, 34 * UNIT, 30 * UNIT)));
   BOOST_REQUIRE_EQUAL(success(), closeenv(SOLANA, 1, digest));
   require_no_shortfall();
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(30 * UNIT), supply());
} FC_LOG_AND_RETHROW()

// No false alarm in flight, a desyndication: it is burned on the depot before the outpost pays it (or
// while the outpost keeps its payout stored as pending), so custody the outpost has not paid out yet only
// adds to the margin. Syndicated 100 and released to alice; she desyndicates 40, leaving 60 outstanding.
// The next syndication of 10 carries 110 while the 40 is unpaid (or pending), and the one after carries 80
// once it is paid: both pass.
BOOST_FIXTURE_TEST_CASE(a_desyndication_in_flight_leaves_the_check_passing, sysio_synd_tester) try {
   const auto     pubkey = ed_bytes(ed_key());
   constexpr auto SVM    = ChainKind::CHAIN_KIND_SVM;
   BOOST_REQUIRE_EQUAL(success(), liq_mint("alice"_n, LIQSOL, 100 * UNIT));   // the released syndication
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, ed_key()));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.desynd_burst = 100 * UNIT}));
   BOOST_REQUIRE_EQUAL(success(), desyndicate("alice"_n, 40 * UNIT));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(60 * UNIT), supply());

   const auto digest = digest_of("envelope-1");
   // Unpaid: the pool still holds the 40, plus the 10 just locked.
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 1, SVM, pubkey, LIQSOL, 10 * UNIT, MSGCH_ACCOUNT,
                                         110 * UNIT));
   BOOST_REQUIRE(console_has(verdict(ONSYND_PATH, EXCESS, 110 * UNIT, 70 * UNIT)));
   require_no_shortfall();
   // Paid: 110 - 40 = 70, plus the next 10 locked.
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 2, SVM, pubkey, LIQSOL, 10 * UNIT, MSGCH_ACCOUNT,
                                         80 * UNIT));
   BOOST_REQUIRE(!console_has(EXCESS));
   require_no_shortfall();
} FC_LOG_AND_RETHROW()

// No false alarm in flight, a desyndication charged a fee: the fee stays in the fee pot as shadow that is
// still outstanding, and only the net is burned and paid. Alice desyndicates 40 at 250 bps: 1 stays in the
// pot and 39 burn, leaving 61 outstanding. The next syndication of 10 carries 110 while the 39 is unpaid,
// and the one after carries 71 + 10 once it is paid: both pass.
BOOST_FIXTURE_TEST_CASE(a_desyndication_with_a_fee_in_flight_leaves_the_check_passing, sysio_synd_tester) try {
   const auto     pubkey = ed_bytes(ed_key());
   constexpr auto SVM    = ChainKind::CHAIN_KIND_SVM;
   BOOST_REQUIRE_EQUAL(success(), liq_mint("alice"_n, LIQSOL, 100 * UNIT));   // the released syndication
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, ed_key()));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.desynd_fee_bps = 250, .desynd_burst = 100 * UNIT}));
   BOOST_REQUIRE_EQUAL(success(), desyndicate("alice"_n, 40 * UNIT));
   BOOST_REQUIRE_EQUAL(UNIT, feepot_balance(LIQSOL));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(61 * UNIT), supply());

   const auto digest = digest_of("envelope-1");
   // Unpaid: the pool still holds the 39, plus the 10 just locked.
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 1, SVM, pubkey, LIQSOL, 10 * UNIT, MSGCH_ACCOUNT,
                                         110 * UNIT));
   BOOST_REQUIRE(console_has(verdict(ONSYND_PATH, EXCESS, 110 * UNIT, 71 * UNIT)));
   require_no_shortfall();
   // Paid: 110 - 39 = 71, plus the next 10 locked -- the fee is still covered.
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 2, SVM, pubkey, LIQSOL, 10 * UNIT, MSGCH_ACCOUNT,
                                         81 * UNIT));
   BOOST_REQUIRE(!console_has(EXCESS));
   require_no_shortfall();
} FC_LOG_AND_RETHROW()

// No false alarm in flight, an INVALID ruling after a partial release: the queue burns the unreleased 6
// and, out of the forfeit, the 4 already released, while the outpost keeps the whole custody. Custody
// here is 25: the envelope's 10 plus the 15 behind dave's bond and carol's shadow, syndicated earlier.
// After the burn 15 is outstanding, and the next syndication of 1 carrying 26 passes.
BOOST_FIXTURE_TEST_CASE(an_invalid_ruling_after_a_partial_release_leaves_the_check_passing,
                        sysio_synd_released_tester) try {
   BOOST_REQUIRE_EQUAL(success(), challenge("carol"_n, SOLANA, LIQSOL, 1));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(25 * UNIT), supply());
   BOOST_REQUIRE_EQUAL(success(), rule(id, false));
   BOOST_REQUIRE_EQUAL(success(), crank());
   const auto envelope = envelope_row(SOLANA, LIQSOL, 1);
   BOOST_REQUIRE_EQUAL(STATE_INVALID, envelope["state"].as_string());
   BOOST_REQUIRE_EQUAL(4 * UNIT, envelope["released"].as_uint64());
   BOOST_REQUIRE_EQUAL(10 * UNIT, envelope["burned"].as_uint64());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(15 * UNIT), supply());

   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 2, digest_of("envelope-2"), 2, ChainKind::CHAIN_KIND_SVM, pubkey,
                                         LIQSOL, UNIT, MSGCH_ACCOUNT, 26 * UNIT));
   BOOST_REQUIRE(console_has(verdict(ONSYND_PATH, EXCESS, 26 * UNIT, 16 * UNIT)));
   require_no_shortfall();
} FC_LOG_AND_RETHROW()

// No false alarm in flight, an INVALID burn: `dropenv` burns a held envelope on the depot while the outpost
// keeps the custody it locked, so the unchanged custody passes against the smaller outstanding.
BOOST_FIXTURE_TEST_CASE(custody_left_behind_by_a_burned_envelope_passes, sysio_synd_tester) try {
   const auto     pubkey = ed_bytes(ed_key());
   constexpr auto SVM    = ChainKind::CHAIN_KIND_SVM;
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest_of("envelope-1"), 1, SVM, pubkey, LIQSOL, 100 * UNIT,
                                         MSGCH_ACCOUNT, 100 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), dropenv(SOLANA, LIQSOL, 1));
   BOOST_REQUIRE_EQUAL(STATE_INVALID, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
   BOOST_REQUIRE_EQUAL(0, supply());

   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 2, digest_of("envelope-2"), 2, SVM, pubkey, LIQSOL, 10 * UNIT,
                                         MSGCH_ACCOUNT, 110 * UNIT));
   BOOST_REQUIRE(console_has(verdict(ONSYND_PATH, EXCESS, 110 * UNIT, 10 * UNIT)));
   require_no_shortfall();
} FC_LOG_AND_RETHROW()

// A released yield is parked in `liqpending` outside the supply until `queueyield`, and the outstanding
// includes it: the outpost claimed it into custody before reporting it. A message whose custody leaves it
// out is a shortfall; one that includes it passes.
BOOST_FIXTURE_TEST_CASE(a_released_yield_counts_before_it_is_queued, sysio_synd_tester) try {
   const auto     pubkey = ed_bytes(ed_key());
   constexpr auto SVM    = ChainKind::CHAIN_KIND_SVM;
   const auto     digest = digest_of("envelope-1");
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 1, SVM, pubkey, LIQSOL, 10 * UNIT, MSGCH_ACCOUNT,
                                         10 * UNIT));
   // A yield of 4 released by the queue: pending, not yet in the supply.
   BOOST_REQUIRE_EQUAL(success(), liq_mintyield(SOLANA, LIQSOL, 4 * UNIT));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(10 * UNIT), supply());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(4 * UNIT), pending());

   // 5 locked; custody without the yield is 15, against 10 + 4 + 5 outstanding.
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 2, SVM, pubkey, LIQSOL, 5 * UNIT, MSGCH_ACCOUNT,
                                         15 * UNIT));
   BOOST_REQUIRE(console_has(verdict(ONSYND_PATH, SHORTFALL, 15 * UNIT, 19 * UNIT)));
   auto rows = mismatch_rows();
   BOOST_REQUIRE_EQUAL(1u, rows.size());
   require_mismatch(rows[0], SOLANA, LIQSOL, 1, 2, MISMATCH_SYNDICATION, 15 * UNIT, 19 * UNIT);

   // 1 more locked with the yield counted: 10 + 4 + 5 + 1, exact.
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 3, SVM, pubkey, LIQSOL, UNIT, MSGCH_ACCOUNT,
                                         20 * UNIT));
   BOOST_REQUIRE(!console_has(SHORTFALL));
   BOOST_REQUIRE(!console_has(EXCESS));
   BOOST_REQUIRE_EQUAL(1u, mismatch_rows().size());
} FC_LOG_AND_RETHROW()

// No false alarm after a refusal: governance returns a desyndication the outpost refused with
// `sysio.liq::recredit`, which grows the supply directly, and the outpost still holds what it never paid.
// The next comparison sees the recredit with nothing recorded in `sysio.synd`: custody 20 matches
// 16 + 4 exactly, where a check that missed the recredit would read an excess of 4.
BOOST_FIXTURE_TEST_CASE(a_recredit_is_seen_by_the_next_comparison, sysio_synd_tester) try {
   const auto     pubkey = ed_bytes(ed_key());
   constexpr auto SVM    = ChainKind::CHAIN_KIND_SVM;
   BOOST_REQUIRE_EQUAL(success(), liq_mint("alice"_n, LIQSOL, 20 * UNIT));   // released: custody 20
   BOOST_REQUIRE_EQUAL(success(), link_svm("alice"_n, ed_key()));
   BOOST_REQUIRE_EQUAL(success(), setconfig(SOLANA, LIQSOL, {.desynd_burst = 100 * UNIT}));
   BOOST_REQUIRE_EQUAL(success(), desyndicate("alice"_n, 4 * UNIT));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(16 * UNIT), supply());
   // The outpost refused it; governance returns the 4 to alice.
   BOOST_REQUIRE_EQUAL(success(), push(SYND_ACCOUNT, synd_abi_ser, SYSIO_ACCOUNT, "refundreturn"_n,
                                       mvo()("request_id", 1)));
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(20 * UNIT), supply());
   const auto ledger_before = ledger_row(SOLANA, LIQSOL);

   // 1 locked: custody 21 against 20 + 1.
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest_of("envelope-1"), 1, SVM, pubkey, LIQSOL, UNIT,
                                         MSGCH_ACCOUNT, 21 * UNIT));
   BOOST_REQUIRE(!console_has(EXCESS));
   require_no_shortfall();
   // The recredit wrote nothing to the ledger's running sums: the check does not read them.
} FC_LOG_AND_RETHROW()

// Review focus 2: only an admitted message is compared. A replayed sequence, or a message dropped for any
// other reason, may carry a stale, lower total; it writes no `mismatch` row and pulls nothing.
BOOST_FIXTURE_TEST_CASE(a_message_not_admitted_is_never_compared, sysio_synd_tester) try {
   const auto     pubkey = ed_bytes(ed_key());
   constexpr auto SVM    = ChainKind::CHAIN_KIND_SVM;
   const auto     digest = digest_of("envelope-1");
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 5, SVM, pubkey, LIQSOL, 10 * UNIT, MSGCH_ACCOUNT,
                                         10 * UNIT));
   // Replays of sequences at or below 5, each carrying a custody of 0.
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 5, SVM, pubkey, LIQSOL, 10 * UNIT, MSGCH_ACCOUNT, 0));
   BOOST_REQUIRE(console_has(DROP_REPLAY));
   require_no_shortfall();
   BOOST_REQUIRE_EQUAL(success(), onyield(SOLANA, 1, digest, 3, 7, LIQSOL, UNIT, MSGCH_ACCOUNT, 0));
   BOOST_REQUIRE(console_has(DROP_YIELD_REPLAY));
   require_no_shortfall();
   // A fresh sequence dropped for another reason: a token of another outpost.
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 6, SVM, pubkey, LIQETH, UNIT, MSGCH_ACCOUNT, 0));
   BOOST_REQUIRE(console_has(DROP_OTHER_CHAIN));
   require_no_shortfall();
   BOOST_REQUIRE_EQUAL(5u, cursor_row(SOLANA)["last_sequence"].as_uint64());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(10 * UNIT), supply());
} FC_LOG_AND_RETHROW()

/// Healthy reports do not erase an incident. Only governance may reconcile it, against live
/// outstanding supply, and reconciliation does not implicitly clear the global emergency stop.
BOOST_FIXTURE_TEST_CASE(incident_reconciliation_is_explicit_and_pair_local, sysio_synd_tester) try {
   const auto pubkey = ed_bytes(ed_key());
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest_of("one"), 1, ChainKind::CHAIN_KIND_SVM,
                                         pubkey, LIQSOL, 10 * UNIT, MSGCH_ACCOUNT, UNIT));
   BOOST_REQUIRE_EQUAL(success(), onyield(SOLANA, 1, digest_of("one"), 2, 2, LIQSOL, UNIT,
                                         MSGCH_ACCOUNT, 10 * UNIT));
   BOOST_REQUIRE_EQUAL(1u, mismatch_rows().size());
   const auto reconcile = [&](name signer, uint64_t reported) { return push(SYND_ACCOUNT, synd_abi_ser,
      signer, "reconcile"_n, mvo()("chain_code", codename_mvo(SOLANA))
      ("token_code", codename_mvo(LIQSOL))("reported", reported)); };
   BOOST_REQUIRE(mentions(reconcile("alice"_n, 10 * UNIT), "missing authority of sysio"));
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("custody still below outstanding"), reconcile(SYSIO_ACCOUNT, 9 * UNIT));
   BOOST_REQUIRE_EQUAL(success(), reconcile(SYSIO_ACCOUNT, 10 * UNIT));
   BOOST_REQUIRE(mismatch_rows().empty());
   BOOST_REQUIRE(cord_pulled());
   BOOST_REQUIRE_EQUAL(wasm_assert_msg("custody incident not found"), reconcile(SYSIO_ACCOUNT, 10 * UNIT));
} FC_LOG_AND_RETHROW()

// Review focus 4: with no sysio.andon deployed, a shortfall is recorded and printed as not pulled, and the
// syndication and its envelope are processed as usual.
BOOST_FIXTURE_TEST_CASE(a_shortfall_without_sysio_andon_is_recorded_and_aborts_nothing,
                        sysio_synd_no_andon_tester) try {
   BOOST_REQUIRE(control->find_account(sysio_system::test_support::andon::account) == nullptr);
   const auto     pubkey = ed_bytes(ed_key());
   constexpr auto SVM    = ChainKind::CHAIN_KIND_SVM;
   const auto     digest = digest_of("envelope-1");
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 1, SVM, pubkey, LIQSOL, 10 * UNIT, MSGCH_ACCOUNT,
                                         9 * UNIT));
   BOOST_REQUIRE(console_has(verdict(ONSYND_PATH, SHORTFALL, 9 * UNIT, 10 * UNIT)));
   BOOST_REQUIRE(console_has("sysio.synd::onsynd" + std::string(CORD_NOT_PULLED)));
   BOOST_REQUIRE(!executed(sysio_system::test_support::andon::account, "pull"_n));
   const auto rows = mismatch_rows();
   BOOST_REQUIRE_EQUAL(1u, rows.size());
   require_mismatch(rows[0], SOLANA, LIQSOL, 1, 1, MISMATCH_SYNDICATION, 9 * UNIT, 10 * UNIT);
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(10 * UNIT), supply());
   BOOST_REQUIRE_EQUAL(10 * UNIT, item_row(1)["amount"].as_uint64());
   BOOST_REQUIRE_EQUAL(success(), onyield(SOLANA, 1, digest, 2, 7, LIQSOL, UNIT, MSGCH_ACCOUNT, 9 * UNIT));
   BOOST_REQUIRE(console_has("sysio.synd::onyield" + std::string(CORD_NOT_PULLED)));
   BOOST_REQUIRE_EQUAL(1u, mismatch_rows().size());
   BOOST_REQUIRE_EQUAL(success(), closeenv(SOLANA, 1, digest));
   BOOST_REQUIRE_EQUAL(STATE_REQUESTED, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
} FC_LOG_AND_RETHROW()

// Review focus 4: with sysio.andon deployed but sysio.synd not registered as a puller, a shortfall is
// recorded and printed as not pulled; `pull` is never sent (it would refuse sysio.synd and abort the
// envelope), and the cord stays clear.
BOOST_FIXTURE_TEST_CASE(a_shortfall_by_an_unregistered_puller_is_recorded_and_aborts_nothing,
                        sysio_synd_unregistered_andon_tester) try {
   const auto     pubkey = ed_bytes(ed_key());
   constexpr auto SVM    = ChainKind::CHAIN_KIND_SVM;
   const auto     digest = digest_of("envelope-1");
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 1, SVM, pubkey, LIQSOL, 10 * UNIT, MSGCH_ACCOUNT,
                                         9 * UNIT));
   BOOST_REQUIRE(console_has(verdict(ONSYND_PATH, SHORTFALL, 9 * UNIT, 10 * UNIT)));
   BOOST_REQUIRE(console_has("sysio.synd::onsynd" + std::string(CORD_NOT_PULLED)));
   BOOST_REQUIRE(!executed(sysio_system::test_support::andon::account, "pull"_n));
   BOOST_REQUIRE(!cord_pulled());
   const auto rows = mismatch_rows();
   BOOST_REQUIRE_EQUAL(1u, rows.size());
   require_mismatch(rows[0], SOLANA, LIQSOL, 1, 1, MISMATCH_SYNDICATION, 9 * UNIT, 10 * UNIT);
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(10 * UNIT), supply());
   BOOST_REQUIRE_EQUAL(success(), closeenv(SOLANA, 1, digest));
   BOOST_REQUIRE_EQUAL(STATE_REQUESTED, envelope_row(SOLANA, LIQSOL, 1)["state"].as_string());
} FC_LOG_AND_RETHROW()

// Review focus 4: a shortfall on a cord already pulled writes its own row, sends no second pull and aborts
// nothing; the cord keeps its first pull.
BOOST_FIXTURE_TEST_CASE(a_shortfall_on_a_pulled_cord_updates_the_incident, sysio_synd_tester) try {
   const auto     pubkey = ed_bytes(ed_key());
   constexpr auto SVM    = ChainKind::CHAIN_KIND_SVM;
   const auto     digest = digest_of("envelope-1");
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 1, SVM, pubkey, LIQSOL, 10 * UNIT, MSGCH_ACCOUNT,
                                         9 * UNIT));
   BOOST_REQUIRE(cord_pulled());
   const auto cord_bytes = [&] {
      return get_row_by_account(sysio_system::test_support::andon::account,
                                sysio_system::test_support::andon::account, "cord"_n, "cord"_n);
   };
   const std::vector<char> first_pull = cord_bytes();

   BOOST_REQUIRE_EQUAL(success(), onyield(SOLANA, 1, digest, 2, 7, LIQSOL, UNIT, MSGCH_ACCOUNT, 8 * UNIT));
   BOOST_REQUIRE(console_has(verdict(ONYIELD_PATH, SHORTFALL, 8 * UNIT, 10 * UNIT)));
   BOOST_REQUIRE(console_has("sysio.synd::onyield" + std::string(CORD_ALREADY)));
   BOOST_REQUIRE(!executed(sysio_system::test_support::andon::account, "pull"_n));
   const auto rows = mismatch_rows();
   BOOST_REQUIRE_EQUAL(1u, rows.size());
   require_mismatch(rows[0], SOLANA, LIQSOL, 1, 2, MISMATCH_YIELD, 8 * UNIT, 10 * UNIT);
   BOOST_REQUIRE_EQUAL(2u, rows[0]["sequence"].as_uint64());
   BOOST_REQUIRE(first_pull == cord_bytes());

   // A later syndication short again records its own row too, and the cord still holds its first pull.
   BOOST_REQUIRE_EQUAL(success(), onsynd(SOLANA, 1, digest, 3, SVM, pubkey, LIQSOL, UNIT, MSGCH_ACCOUNT, UNIT));
   BOOST_REQUIRE(console_has("sysio.synd::onsynd" + std::string(CORD_ALREADY)));
   BOOST_REQUIRE_EQUAL(1u, mismatch_rows().size());
   BOOST_REQUIRE(first_pull == cord_bytes());
   BOOST_REQUIRE_EQUAL(static_cast<int64_t>(11 * UNIT), supply());
   BOOST_REQUIRE_EQUAL(success(), closeenv(SOLANA, 1, digest));
   BOOST_REQUIRE(console_has(QUEUE_FROZEN));
} FC_LOG_AND_RETHROW()


// These scenarios start with custody-backed bootstrap credits. Unlike the narrow
// bond fixture's underwrite(), accepting a request never creates extra collateral.
struct generic_synd_tester : sysio_synd_tester {
   static constexpr uint64_t Bootstrap = 1'000 * UNIT;
   static constexpr uint32_t Window = 3600;
   std::array<external::chain, 2> outposts{external::chain(external::First), external::chain(external::Second)};
   std::array<std::vector<char>, 2> users;

   generic_synd_tester() : sysio_synd_tester(false) {
      for (size_t i = 0; i < outposts.size(); ++i) {
         const auto& a = outposts[i].asset();
         BOOST_REQUIRE_EQUAL(success(), regchain(a.kind, a.chain, a.id));
         BOOST_REQUIRE_EQUAL(success(), regtoken(a.token, a.kind, a.chain));
         BOOST_REQUIRE_EQUAL(success(), create_shadow(sym(i), a.chain, a.token));
         for (const name account : {"alice"_n, "carol"_n, "dave"_n}) {
            const auto key = a.kind == ChainKind::CHAIN_KIND_EVM ? em_key() : ed_key();
            const auto bytes = a.kind == ChainKind::CHAIN_KIND_EVM ? em_bytes(key) : ed_bytes(key);
            BOOST_REQUIRE_EQUAL(success(), push(AUTHEX_ACCOUNT, authex_abi_ser, AUTHEX_ACCOUNT, "recordlink"_n,
               mvo()("account", account)("chain_kind", a.kind)("pub_key", key)
                  ("native_address", native_address_of(key))));
            if (account == "alice"_n) users[i] = bytes;
            else {
               outposts[i].donate(Bootstrap);
               BOOST_REQUIRE_EQUAL(success(), importsynd(a.chain, a.token, {credit(bytes, Bootstrap)}));
            }
         }
         BOOST_REQUIRE_EQUAL(success(), setconfig(a.chain, a.token,
            {.synd_burst = 3 * UNIT, .synd_refill = 3 * UNIT, .window_sec = Window}));
      }
      BOOST_REQUIRE_EQUAL(success(), importdone());
   }
   symbol sym(size_t i) const { return symbol::from_string(std::string("9,") + outposts[i].asset().token); }
   void conservation() {
      for (size_t i = 0; i < outposts.size(); ++i) {
         const auto token = sym(i);
         BOOST_REQUIRE_EQUAL(supply(token), liq_balance("alice"_n, token) + liq_balance("carol"_n, token) +
            liq_balance("dave"_n, token) + liq_balance("bob"_n, token) + liq_balance(SWAP_ACCOUNT, token) +
            liq_balance(LIQ_ACCOUNT, token) + liq_balance(SYND_ACCOUNT, token) + liq_balance(BOND_ACCOUNT, token));
         BOOST_REQUIRE_LE(uint64_t(supply(token) + pending(token)), outposts[i].custody());
      }
      require_no_shortfall();
   }
   void accept(uint64_t request) {
      BOOST_REQUIRE_EQUAL(success(), push(BOND_ACCOUNT, bond_abi_ser, "dave"_n, "accept"_n,
         mvo()("underwriter", "dave"_n)("request_id", request)("amount", bond_request(request)["covered"])));
      conservation();
   }
};

// Fixed seeds make failures reproducible; generated choices vary chain order,
// amount, challenge outcome, freeze timing, release count, and return amount.
// Each round consumes the preceding round's balances and sequence cursors.
BOOST_AUTO_TEST_CASE(generated_generic_custody_bond_challenge_and_return_sequences) {
   constexpr std::array<uint32_t, 3> Seeds{0x51a7, 0xc011a7, 0xb0ad};
   constexpr size_t Rounds = 8;
   for (const auto seed : Seeds) {
      generic_synd_tester t;
      std::mt19937 random(seed);
      uint32_t epoch = 1;
      uint64_t return_id = 0;
      for (size_t step = 0; step < Rounds; ++step) {
         BOOST_TEST_CONTEXT("seed=" << seed << " step=" << step) {
            const size_t i = random() % t.outposts.size();
            auto& external = t.outposts[i];
            const auto& a = external.asset();
            const auto token = t.sym(i);
            const auto other_token = t.sym(1 - i);
            const auto other_state = [&] {
               return std::array<int64_t, 6>{t.supply(other_token), t.liq_balance("alice"_n, other_token),
                  t.liq_balance("carol"_n, other_token), t.liq_balance("dave"_n, other_token),
                  t.liq_balance(t.SYND_ACCOUNT, other_token), t.liq_balance(t.BOND_ACCOUNT, other_token)};
            };
            const auto untouched = other_state();
            const uint64_t amount = (7 + random() % 9) * t.UNIT;
            const bool invalid = step % 3 == 0;
            const bool challenged = invalid || (random() % 2 == 0);
            const auto before = t.liq_balance("alice"_n, token);
            const auto supply_before = t.supply(token);
            BOOST_REQUIRE_EQUAL(t.success(), t.set_epoch(epoch));
            const auto report = external.deposit(t.users[i], amount);
            sysio::opp::attestations::SyndicateLIQ deposit;
            BOOST_REQUIRE(deposit.ParseFromString(report.second));
            const auto digest = t.digest_of(std::to_string(seed) + ":" + std::to_string(step));
            const auto ingest = [&] {
               return t.onsynd(a.chain, epoch, digest, deposit.sequence(), a.kind, t.users[i], a.token,
                  amount, t.MSGCH_ACCOUNT, deposit.total_syndicated());
            };
            BOOST_REQUIRE_EQUAL(t.success(), ingest());
            t.conservation();
            // Authenticated duplicate delivery cannot mint a second time.
            BOOST_REQUIRE_EQUAL(t.success(), ingest());
            BOOST_REQUIRE_EQUAL(supply_before + int64_t(amount), t.supply(token));
            BOOST_REQUIRE_EQUAL(t.success(), t.closeenv(a.chain, epoch, digest));
            const auto request = t.envelope_row(a.chain, a.token, epoch)["request_id"].as_uint64();
            t.accept(request);
            BOOST_REQUIRE_EQUAL(t.success(), t.crank());
            const auto released = t.liq_balance("alice"_n, token) - before;
            BOOST_REQUIRE_EQUAL(int64_t(3 * t.UNIT), released);
            t.conservation();
            if (random() % 2 == 0) {
               BOOST_REQUIRE_EQUAL(t.success(), t.andon_pull());
               const auto frozen_supply = t.supply(token);
               BOOST_REQUIRE_EQUAL(t.frozen(), t.desyndicate("alice"_n, t.UNIT, token));
               BOOST_REQUIRE_EQUAL(frozen_supply, t.supply(token));
               BOOST_REQUIRE_EQUAL(t.success(), t.andon_clear());
            }
            if (challenged) {
               BOOST_REQUIRE_EQUAL(t.success(), t.challenge("carol"_n, a.chain, a.token, epoch));
               BOOST_REQUIRE_EQUAL(t.success(), t.crank());
               BOOST_REQUIRE_EQUAL(before + released, t.liq_balance("alice"_n, token));
               BOOST_REQUIRE_EQUAL(t.success(), t.rule(request, !invalid));
            } else {
               t.produce_block(fc::seconds(t.Window + 1));
               BOOST_REQUIRE_EQUAL(t.success(), t.push(t.BOND_ACCOUNT, t.bond_abi_ser, "carol"_n, "approve"_n,
                  mvo()("request_id", request)));
            }
            if (invalid) {
               BOOST_REQUIRE_EQUAL(t.success(), t.crank());
               BOOST_REQUIRE_EQUAL(supply_before, t.supply(token));
               BOOST_REQUIRE_EQUAL(STATE_INVALID, t.envelope_row(a.chain, a.token, epoch)["state"].as_string());
               BOOST_REQUIRE_EQUAL(t.success(), t.bond_claim(request, "carol"_n));
            } else {
               // Drain by advancing real queue budgets; bounded iteration catches stalls.
               for (size_t drain = 0; drain < 6; ++drain) {
                  BOOST_REQUIRE_EQUAL(t.success(), t.set_epoch(++epoch));
                  BOOST_REQUIRE_EQUAL(t.success(), t.crank());
                  t.conservation();
               }
               BOOST_REQUIRE_EQUAL(before + int64_t(amount), t.liq_balance("alice"_n, token));
               BOOST_REQUIRE_EQUAL(t.success(), t.bond_claim(request, "dave"_n));
            }
            t.conservation();
            const uint64_t returned = (1 + random() % 3) * t.UNIT;
            BOOST_REQUIRE_EQUAL(t.success(), t.desyndicate("alice"_n, returned, token));
            const auto outbound = queued_desyndication(t, ++return_id);
            BOOST_REQUIRE_EQUAL(returned, outbound.amount().amount());
            external.freeze();
            BOOST_REQUIRE(!external.receive(outbound));
            BOOST_REQUIRE(!external.receive(outbound));
            external.clear();
            BOOST_REQUIRE(external.settle(outbound.request_id()));
            BOOST_REQUIRE(!external.receive(outbound));
            t.conservation();
            BOOST_REQUIRE(untouched == other_state());
            ++epoch;
         }
      }
   }
}


BOOST_FIXTURE_TEST_CASE(generic_parked_credit_yield_conversion_and_fee_rounded_return, generic_synd_tester) {
   constexpr uint64_t Principal = 101 * UNIT + 17;
   constexpr uint64_t Yield = 10 * UNIT;
   constexpr uint32_t FeeBps = 37;
   constexpr uint64_t PoolSeed = 1'000 * UNIT;
   constexpr uint32_t Horizon = 86'400, DepthBps = 300;
   constexpr int64_t ClipFloor = 1000;
   BOOST_REQUIRE_EQUAL(success(), push(LIQ_ACCOUNT, liq_abi_ser, SYSIO_ACCOUNT, "setkicker"_n, mvo()("bps", 0)));
   for (size_t i = 0; i < outposts.size(); ++i) {
      BOOST_TEST_CONTEXT("chain=" << outposts[i].asset().chain) {
         auto& external = outposts[i];
         const auto& a = external.asset();
         const auto token = sym(i);
         const auto pair = symbol::from_string(i == 0 ? "9,POOLA" : "9,POOLB");
         external.donate(PoolSeed);
         BOOST_REQUIRE_EQUAL(success(), create_pool(a.chain, a.token, pair, PoolSeed, PoolSeed,
            30, 0, Horizon, DepthBps, ClipFloor));
         BOOST_REQUIRE_EQUAL(success(), setconfig(a.chain, a.token,
            {.synd_fee_bps = FeeBps, .desynd_fee_bps = FeeBps, .window_sec = Window}));
         const auto private_key = fc::crypto::private_key::generate(a.kind == ChainKind::CHAIN_KIND_EVM
            ? fc::crypto::private_key::key_type::em : fc::crypto::private_key::key_type::ed);
         const auto key = private_key.get_public_key();
         const auto bytes = a.kind == ChainKind::CHAIN_KIND_EVM ? em_bytes(key) : ed_bytes(key);
         sysio::opp::attestations::SyndicateLIQ deposit;
         BOOST_REQUIRE(deposit.ParseFromString(external.deposit(bytes, Principal).second));
         sysio::opp::attestations::LIQYield report;
         BOOST_REQUIRE(report.ParseFromString(external.yield(Yield).second));
         const auto digest = digest_of(a.chain);
         constexpr uint32_t Epoch = 1;
         BOOST_REQUIRE_EQUAL(success(), onsynd(a.chain, Epoch, digest, deposit.sequence(), a.kind, bytes,
            a.token, Principal, MSGCH_ACCOUNT, deposit.total_syndicated()));
         BOOST_REQUIRE_EQUAL(success(), onyield(a.chain, Epoch, digest, report.sequence(), report.epoch(),
            a.token, Yield, MSGCH_ACCOUNT, report.total_syndicated()));
         BOOST_REQUIRE_EQUAL(success(), closeenv(a.chain, Epoch, digest));
         BOOST_REQUIRE_EQUAL(0, pending(token));
         BOOST_REQUIRE_EQUAL(0u, parked_balance(a.token, a.kind, bytes));
         accept(envelope_row(a.chain, a.token, Epoch)["request_id"].as_uint64());
         BOOST_REQUIRE_EQUAL(success(), crank());
         const uint64_t fee = Principal * FeeBps / BPS_DENOMINATOR;
         BOOST_REQUIRE_EQUAL(Principal - fee, parked_balance(a.token, a.kind, bytes));
         BOOST_REQUIRE_EQUAL(fee, feepot_balance(a.token));
         BOOST_REQUIRE_EQUAL(int64_t(Yield), pending(token));
         conservation();
         // Link notification crosses authex -> synd -> liq and consumes the parked row once.
         BOOST_REQUIRE_EQUAL(success(), createlink("bob"_n, private_key, a.kind));
         BOOST_REQUIRE_EQUAL(int64_t(Principal - fee), liq_balance("bob"_n, token));
         BOOST_REQUIRE_EQUAL(0u, parked_balance(a.token, a.kind, bytes));
         BOOST_REQUIRE_EQUAL(success(), sweep("bob"_n, a.kind));
         BOOST_REQUIRE_EQUAL(int64_t(Principal - fee), liq_balance("bob"_n, token));
         BOOST_REQUIRE_EQUAL(success(), push(LIQ_ACCOUNT, liq_abi_ser, "alice"_n, "queueyield"_n,
            mvo()("sym", token.to_symbol_code())));
         BOOST_REQUIRE_EQUAL(0, pending(token));
         conservation();
         produce_blocks(40);
         BOOST_REQUIRE_EQUAL(success(), push(SWAP_ACCOUNT, swap_abi_ser, "alice"_n, "tickyield"_n,
            mvo()("pair_token", pair.to_symbol_code())));
         const auto owed = liq_owed("bob"_n, token);
         BOOST_REQUIRE_GT(owed, 0);
         const auto wire_before = wire_balance("bob"_n);
         BOOST_REQUIRE_EQUAL(success(), liq_claim("bob"_n, token));
         BOOST_REQUIRE_EQUAL(wire_before + owed, wire_balance("bob"_n));
         BOOST_REQUIRE_EQUAL(0, liq_owed("bob"_n, token));
         constexpr uint64_t Return = 3 * UNIT + 29;
         BOOST_REQUIRE_EQUAL(success(), desyndicate("bob"_n, Return, token));
         const auto outbound = queued_desyndication(*this, i + 1);
         BOOST_REQUIRE_EQUAL(Return - Return * FeeBps / BPS_DENOMINATOR, outbound.amount().amount());
         BOOST_REQUIRE(external.receive(outbound));
         BOOST_REQUIRE(!external.receive(outbound));
         conservation();
      }
   }
}

BOOST_FIXTURE_TEST_CASE(generic_custody_shortfall_freezes_and_repair_resumes, generic_synd_tester) {
   auto& external = outposts.front();
   const auto& a = external.asset();
   external.lose(3 * UNIT);
   sysio::opp::attestations::LIQYield report;
   BOOST_REQUIRE(report.ParseFromString(external.yield(UNIT).second));
   const auto digest = digest_of("generic shortfall");
   BOOST_REQUIRE_EQUAL(success(), onyield(a.chain, 1, digest, report.sequence(), report.epoch(), a.token,
      UNIT, MSGCH_ACCOUNT, report.total_syndicated()));
   BOOST_REQUIRE(cord_pulled());
   BOOST_REQUIRE_EQUAL(1u, mismatch_rows().size());
   BOOST_REQUIRE_EQUAL(report.total_syndicated(), mismatch_rows().front()["reported"].as_uint64());
   BOOST_REQUIRE_EQUAL(uint64_t(supply(sym(0))), mismatch_rows().front()["expected"].as_uint64());
   BOOST_REQUIRE_EQUAL(success(), closeenv(a.chain, 1, digest));
   const auto id = envelope_row(a.chain, a.token, 1)["request_id"].as_uint64();
   BOOST_REQUIRE_EQUAL(success(), push(BOND_ACCOUNT, bond_abi_ser, "dave"_n, "accept"_n,
      mvo()("underwriter", "dave"_n)("request_id", id)("amount", UNIT)));
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(0, pending(sym(0)));
   external.donate(3 * UNIT);
   BOOST_REQUIRE_EQUAL(success(), andon_clear());
   BOOST_REQUIRE_EQUAL(success(), crank());
   BOOST_REQUIRE_EQUAL(int64_t(UNIT), pending(sym(0)));
   BOOST_REQUIRE_EQUAL(uint64_t(supply(sym(0)) + pending(sym(0))), external.custody());
   BOOST_REQUIRE(!cord_pulled());
   // Clearing the cord does not erase incident evidence or touch the other chain.
   BOOST_REQUIRE_EQUAL(1u, mismatch_rows().size());
   BOOST_REQUIRE_EQUAL(0, pending(sym(1)));
}

// Governance resolves truth without collateral; providers supply collateral and
// retain their challenge window. Compare the downstream economic trace, not the
// deliberately different bond state or escrow owner.
BOOST_AUTO_TEST_CASE(generic_governance_and_provider_release_have_identical_settlement) {
   constexpr uint64_t Principal = 8 * external::Unit + 17;
   constexpr uint64_t Yield = 2 * external::Unit;
   constexpr uint64_t Return = external::Unit + 29;
   constexpr uint32_t FeeBps = 37;
   constexpr uint64_t Tranche = 3 * external::Unit;
   constexpr uint64_t PrincipalFee = 2 * (Tranche * FeeBps / BPS_DENOMINATOR) +
      (Principal - 2 * Tranche) * FeeBps / BPS_DENOMINATOR;
   for (size_t i = 0; i < external::Assets.size(); ++i) {
      for (const bool parked : {false, true}) {
         std::vector<std::array<int64_t, 7>> provider_trace;
         for (const bool governance : {false, true}) {
            BOOST_TEST_CONTEXT("chain=" << external::Assets[i].chain << " parked=" << parked
                               << " governance=" << governance) {
               generic_synd_tester t;
               auto& outpost = t.outposts[i];
               const auto& a = outpost.asset();
               const auto token = t.sym(i);
               const auto private_key = fc::crypto::private_key::generate(a.kind == ChainKind::CHAIN_KIND_EVM
                  ? fc::crypto::private_key::key_type::em : fc::crypto::private_key::key_type::ed);
               const auto key = private_key.get_public_key();
               const auto bytes = parked ? (a.kind == ChainKind::CHAIN_KIND_EVM ? t.em_bytes(key) : t.ed_bytes(key))
                                         : t.users[i];
               const auto holder = parked ? "bob"_n : "alice"_n;
               BOOST_REQUIRE_EQUAL(t.success(), t.setconfig(a.chain, a.token,
                  {.synd_fee_bps = FeeBps, .desynd_fee_bps = FeeBps,
                   .synd_burst = 3 * external::Unit, .synd_refill = 3 * external::Unit, .window_sec = t.Window}));
               const auto digest = t.digest_of("resolution parity");
               sysio::opp::attestations::SyndicateLIQ deposit;
               sysio::opp::attestations::LIQYield yield;
               BOOST_REQUIRE(deposit.ParseFromString(outpost.deposit(bytes, Principal).second));
               BOOST_REQUIRE(yield.ParseFromString(outpost.yield(Yield).second));
               BOOST_REQUIRE_EQUAL(t.success(), t.set_epoch(1));
               BOOST_REQUIRE_EQUAL(t.success(), t.onsynd(a.chain, 1, digest, deposit.sequence(), a.kind,
                  bytes, a.token, Principal, t.MSGCH_ACCOUNT, deposit.total_syndicated()));
               BOOST_REQUIRE_EQUAL(t.success(), t.onyield(a.chain, 1, digest, yield.sequence(), yield.epoch(),
                  a.token, Yield, t.MSGCH_ACCOUNT, yield.total_syndicated()));
               BOOST_REQUIRE_EQUAL(t.success(), t.closeenv(a.chain, 1, digest));
               const auto request = t.envelope_row(a.chain, a.token, 1)["request_id"].as_uint64();
               BOOST_REQUIRE_EQUAL(t.success(), t.crank());
               BOOST_REQUIRE_EQUAL(0, t.liq_balance(holder, token));
               BOOST_REQUIRE_EQUAL(0u, t.parked_balance(a.token, a.kind, bytes));
               BOOST_REQUIRE_EQUAL(0u, t.envelope_row(a.chain, a.token, 1)["released"].as_uint64());
               if (governance) {
                  BOOST_REQUIRE_NE(t.success(), t.push(t.BOND_ACCOUNT, t.bond_abi_ser, "alice"_n,
                     "rslvvalid"_n, mvo()("request_id", request)));
                  BOOST_REQUIRE_EQUAL(t.success(), t.rule(request, true));
                  BOOST_REQUIRE_EQUAL(0u, t.bond_request(request)["bonded"].as_uint64());
               } else {
                  t.accept(request);
                  BOOST_REQUIRE_NE(t.success(), t.push(t.BOND_ACCOUNT, t.bond_abi_ser, "alice"_n,
                     "approve"_n, mvo()("request_id", request)));
               }
               std::vector<std::array<int64_t, 7>> trace;
               const auto capture = [&] {
                  t.conservation();
                  trace.push_back({t.supply(token), t.pending(token), t.liq_balance(holder, token),
                     int64_t(t.parked_balance(a.token, a.kind, bytes)), int64_t(t.feepot_balance(a.token)),
                     int64_t(t.envelope_row(a.chain, a.token, 1)["released"].as_uint64()),
                     int64_t(outpost.custody())});
               };
               BOOST_REQUIRE_EQUAL(t.success(), t.andon_pull());
               BOOST_REQUIRE_EQUAL(t.success(), t.crank());
               BOOST_REQUIRE_EQUAL(0u, t.envelope_row(a.chain, a.token, 1)["released"].as_uint64());
               // conservation() also requires the cord clear, so capture after clear.
               BOOST_REQUIRE_EQUAL(t.success(), t.andon_clear());
               for (uint32_t epoch = 2; epoch <= 6; ++epoch) {
                  BOOST_REQUIRE_EQUAL(t.success(), t.set_epoch(epoch));
                  BOOST_REQUIRE_EQUAL(t.success(), t.crank());
                  capture();
                  BOOST_REQUIRE_EQUAL(t.success(), t.crank());
                  capture(); // same epoch must not replenish the bucket
               }
               BOOST_REQUIRE_EQUAL(STATE_DONE, t.envelope_row(a.chain, a.token, 1)["state"].as_string());
               BOOST_REQUIRE_EQUAL(Principal + Yield,
                  t.envelope_row(a.chain, a.token, 1)["released"].as_uint64());
               if (!governance) {
                  t.produce_block(fc::seconds(t.Window + 1));
                  BOOST_REQUIRE_EQUAL(t.success(), t.push(t.BOND_ACCOUNT, t.bond_abi_ser, "alice"_n,
                     "approve"_n, mvo()("request_id", request)));
                  BOOST_REQUIRE_EQUAL(t.success(), t.bond_claim(request, "dave"_n));
               }
               if (parked) BOOST_REQUIRE_EQUAL(t.success(), t.createlink(holder, private_key, a.kind));
               BOOST_REQUIRE_EQUAL(0u, t.parked_balance(a.token, a.kind, bytes));
               BOOST_REQUIRE_EQUAL(int64_t(Principal - PrincipalFee), t.liq_balance(holder, token));
               BOOST_REQUIRE_EQUAL(PrincipalFee, t.feepot_balance(a.token));
               BOOST_REQUIRE_EQUAL(int64_t(Yield), t.pending(token));
               BOOST_REQUIRE_EQUAL(int64_t(t.Bootstrap), t.liq_balance("dave"_n, token));
               capture();
               BOOST_REQUIRE_EQUAL(t.success(), t.desyndicate(holder, Return, token));
               const auto outbound = queued_desyndication(t, 1);
               BOOST_REQUIRE_EQUAL(a.kind, outbound.user().kind());
               BOOST_REQUIRE_EQUAL(std::string(bytes.begin(), bytes.end()), outbound.user().address());
               BOOST_REQUIRE_EQUAL(Return - Return * FeeBps / BPS_DENOMINATOR, outbound.amount().amount());
               BOOST_REQUIRE(outpost.receive(outbound));
               BOOST_REQUIRE(!outpost.receive(outbound));
               capture();
               if (governance) {
                  constexpr std::array<std::string_view, 7> Fields{
                     "supply", "pending yield", "recipient balance", "parked credit",
                     "fee pot", "released", "external custody"};
                  BOOST_REQUIRE_EQUAL(provider_trace.size(), trace.size());
                  for (size_t checkpoint = 0; checkpoint < trace.size(); ++checkpoint)
                     for (size_t field = 0; field < Fields.size(); ++field)
                        BOOST_TEST_CONTEXT("checkpoint=" << checkpoint << " field=" << Fields[field]) {
                           BOOST_REQUIRE_EQUAL(provider_trace[checkpoint][field], trace[checkpoint][field]);
                        }
               } else provider_trace = trace;
            }
         }
      }
   }
}

BOOST_AUTO_TEST_SUITE_END()
