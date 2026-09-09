#include <sysio.chains/sysio.chains.hpp>
#include <sysio.epoch/sysio.epoch.hpp>
#include <sysio.opp.common/registry_metadata.hpp>
#include <sysio.opp.common/wire_asset.hpp>

namespace sysio {

namespace {

// System-owned rows bill to the sysio RAM pool, not this contract account (privileged-contract
// model, as sysio.token uses): the account stays finite at code+abi size; growth draws from the pool.
constexpr name ram_payer = "sysio"_n;

uint64_t current_time_ms() {
   return static_cast<uint64_t>(current_time_point().sec_since_epoch()) * 1000;
}

uint32_t get_current_epoch_index() {
   sysio::epoch::epochstate_t es(chains::EPOCH_ACCOUNT);
   if (!es.exists()) return 0;
   return es.get().current_epoch_index;
}

bool is_bootstrap_window() {
   return get_current_epoch_index() == 0;
}

void require_priv_caller() {
   require_auth(current_receiver());
   sysio::check(sysio::is_privileged(current_receiver()),
                "sysio.chains: privileged account required");
}

// ---------------------------------------------------------------------------
//  Remote-address format validation
//
//  These addresses are consensus facts: every batch operator and underwriter
//  reads the same row, so one malformed value breaks relay for the whole
//  network rather than for a single misconfigured node. Validate the format at
//  the ingress boundary, where the caller can still fix it.
//
//  Empty is allowed -- a chain may be registered before its remote contracts
//  are deployed and filled in later via `setoutpost`; both daemons fail closed
//  and skip a row whose address they need but do not have. The exception is a
//  field that is structurally meaningless for the kind, which must be empty.
// ---------------------------------------------------------------------------

constexpr size_t EVM_ADDR_LEN = 42;    // "0x" + 20 bytes hex
constexpr size_t SVM_ADDR_MIN = 32;    // base58 of a 32-byte pubkey, lower bound
constexpr size_t SVM_ADDR_MAX = 44;    // base58 of a 32-byte pubkey, upper bound

// Upper bound for a kind whose address format is not constrained below. The
// EVM and SVM checks are far tighter; this exists only so an unrecognised
// future kind cannot park an unbounded string in `sysio`-billed state, the
// same concern `registry_metadata.hpp` bounds `name` and `description` for.
constexpr size_t ADDR_MAX_BYTES = 128;

bool is_hex_digit(char c) {
   return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/// Bitcoin/Solana base58 alphabet -- [1-9A-HJ-NP-Za-km-z] (excludes 0, O, I, l).
bool is_base58_char(char c) {
   return (c >= '1' && c <= '9')
       || (c >= 'A' && c <= 'H') || (c >= 'J' && c <= 'N') || (c >= 'P' && c <= 'Z')
       || (c >= 'a' && c <= 'k') || (c >= 'm' && c <= 'z');
}

void check_evm_addr(const std::string& addr, const char* label) {
   sysio::check(addr.size() == EVM_ADDR_LEN && addr[0] == '0' && addr[1] == 'x',
                std::string("sysio.chains: ") + label + " must be a 0x-prefixed 20-byte hex address");
   for (size_t i = 2; i < addr.size(); ++i) {
      sysio::check(is_hex_digit(addr[i]),
                   std::string("sysio.chains: ") + label + " contains a non-hex character");
   }
}

void check_svm_addr(const std::string& addr, const char* label) {
   sysio::check(addr.size() >= SVM_ADDR_MIN && addr.size() <= SVM_ADDR_MAX,
                std::string("sysio.chains: ") + label + " must be a base58 program id (32-44 chars)");
   for (char c : addr) {
      sysio::check(is_base58_char(c),
                   std::string("sysio.chains: ") + label + " contains a non-base58 character");
   }
}

void check_empty(const std::string& addr, const char* label, const char* why) {
   sysio::check(addr.empty(), std::string("sysio.chains: ") + label + " must be empty -- " + why);
}

/// Validate an `outpost_addrs` set against the chain kind. Non-empty values
/// must match the kind's format; structurally-unused fields must be empty.
void validate_outpost_addrs(opp::types::ChainKind kind, const outpost_addrs& o) {
   // Applies to every kind, including ones with no format rule yet.
   for (const auto* f : {&o.opp_addr, &o.opp_inbound_addr,
                         &o.operator_registry_addr, &o.source_deposit_addr}) {
      sysio::check(f->size() <= ADDR_MAX_BYTES,
                   "sysio.chains: outpost address exceeds "
                   + std::to_string(ADDR_MAX_BYTES) + " bytes");
   }

   switch (kind) {
      case opp::types::CHAIN_KIND_WIRE: {
         constexpr auto why = "the WIRE depot self-row has no remote deployment";
         check_empty(o.opp_addr,               "opp_addr",               why);
         check_empty(o.opp_inbound_addr,       "opp_inbound_addr",       why);
         check_empty(o.operator_registry_addr, "operator_registry_addr", why);
         check_empty(o.source_deposit_addr,    "source_deposit_addr",    why);
         break;
      }
      case opp::types::CHAIN_KIND_EVM:
         // Each role is its own contract on an EVM chain.
         if (!o.opp_addr.empty())               check_evm_addr(o.opp_addr,               "opp_addr");
         if (!o.opp_inbound_addr.empty())       check_evm_addr(o.opp_inbound_addr,       "opp_inbound_addr");
         if (!o.operator_registry_addr.empty()) check_evm_addr(o.operator_registry_addr, "operator_registry_addr");
         if (!o.source_deposit_addr.empty())    check_evm_addr(o.source_deposit_addr,    "source_deposit_addr");
         break;
      case opp::types::CHAIN_KIND_SVM: {
         // One program serves every role; the daemons substitute opp_addr.
         constexpr auto why = "an SVM outpost is a single program, named by opp_addr";
         if (!o.opp_addr.empty()) check_svm_addr(o.opp_addr, "opp_addr");
         check_empty(o.opp_inbound_addr,       "opp_inbound_addr",       why);
         check_empty(o.operator_registry_addr, "operator_registry_addr", why);
         check_empty(o.source_deposit_addr,    "source_deposit_addr",    why);
         break;
      }
      default:
         // Future kinds: bounded above, no format constraint yet.
         break;
   }
}

} // namespace

void chains::regchain(opp::types::ChainKind kind,
                      sysio::slug_name       code,
                      uint32_t              external_chain_id,
                      std::string           name,
                      std::string           description,
                      outpost_addrs         outpost) {
   require_priv_caller();

   sysio::check(kind != opp::types::CHAIN_KIND_UNKNOWN,
                "sysio.chains: kind must not be UNKNOWN");
   // Both strings persist into a `sysio`-billed row -- bound them before emplace.
   opp::registry::check_metadata(name, description, "sysio.chains");
   validate_outpost_addrs(kind, outpost);

   chains_t tbl(get_self());
   chain_key pk{code};
   sysio::check(tbl.find(pk) == tbl.end(),
                "sysio.chains: chain code already registered");

   // Enforce: the code `WIRE` and `CHAIN_KIND_WIRE` are reserved FOR EACH OTHER, and the
   // depot self-row is unique.
   //
   // Forward: `is_depot` is derived from the KIND alone below, so kind=WIRE must carry the
   // canonical code or a row could claim depot identity under any code (e.g. `FAKE`) --
   // previously only the cardinality half of bootstrap invariant V3 was on-chain and the code
   // rested entirely on the off-chain config validator.
   //
   // Inverse: chain codes are unique and there is NO erase action, so a non-WIRE row
   // registered under the code `WIRE` would permanently squat the depot's identity and leave
   // the canonical row unregisterable -- bricking bootstrap with no on-chain recovery. Both
   // directions are needed; the forward check alone still admits `regchain(EVM, "WIRE", ...)`.
   if (kind == opp::types::CHAIN_KIND_WIRE) {
      sysio::check(code == opp::wire::chain_code,
                   "sysio.chains: a WIRE chain must use the code WIRE");

      auto by_kind_idx = tbl.template get_index<"bykind"_n>();
      const auto wire_kind_value = magic_enum::enum_integer(opp::types::CHAIN_KIND_WIRE);
      sysio::check(by_kind_idx.lower_bound(wire_kind_value) == by_kind_idx.upper_bound(wire_kind_value),
                   "sysio.chains: a WIRE chain (depot self-row) already exists");
   } else {
      sysio::check(code != opp::wire::chain_code,
                   "sysio.chains: the code WIRE is reserved for the depot self-row");
   }

   // Enforce: EVM rows are unique per `external_chain_id`. The pair (kind, external_chain_id)
   // is stamped into every outbound envelope's route endpoints (`sysio.msgch::buildenv`) and is
   // what an EVM outpost verifies against its own `block.chainid`, so it must stay injective
   // across EVM rows.
   if (kind == opp::types::CHAIN_KIND_EVM) {
      auto by_extid_idx = tbl.template get_index<"byextid"_n>();
      for (auto it = by_extid_idx.lower_bound(external_chain_id);
           it != by_extid_idx.end() && it->external_chain_id == external_chain_id; ++it) {
         sysio::check(it->kind != opp::types::CHAIN_KIND_EVM,
                      "sysio.chains: an EVM chain with this external_chain_id already exists");
      }
   }

   // Enforce: at most one SVM row. The wire destination binding carries only
   // {kind, external_chain_id} and Solana clusters have no numeric chain id, so the Solana
   // outpost gate can verify the KIND alone — a second SVM row would be indistinguishable on
   // the wire and reopen the misdelivery gap the binding closes. Fail closed until the
   // registry slug is carried in the envelope and validated by the outpost.
   if (kind == opp::types::CHAIN_KIND_SVM) {
      auto by_kind_idx = tbl.template get_index<"bykind"_n>();
      const auto svm_kind_value = magic_enum::enum_integer(opp::types::CHAIN_KIND_SVM);
      sysio::check(by_kind_idx.lower_bound(svm_kind_value) == by_kind_idx.upper_bound(svm_kind_value),
                   "sysio.chains: an SVM chain is already registered and the destination binding "
                   "cannot distinguish a second one");
   }

   const auto now = current_time_ms();
   const bool bootstrap = is_bootstrap_window();

   tbl.emplace(ram_payer, pk, chain_row{
      .code               = code,
      .kind               = kind,
      .external_chain_id  = external_chain_id,
      .name               = std::move(name),
      .description        = std::move(description),
      .is_depot           = (kind == opp::types::CHAIN_KIND_WIRE),
      .active             = bootstrap,
      .registered_at_ms   = now,
      .activated_at_ms    = bootstrap ? now : 0,
      .outpost            = std::move(outpost),
   });
}

void chains::activchain(sysio::slug_name code) {
   require_priv_caller();

   chains_t tbl(get_self());
   chain_key pk{code};
   auto it = tbl.find(pk);
   sysio::check(it != tbl.end(), "sysio.chains: chain code not registered");
   sysio::check(!it->active, "sysio.chains: chain is already active");

   tbl.modify(ram_payer, pk, [&](auto& row) {
      row.active          = true;
      row.activated_at_ms = current_time_ms();
   });
}

void chains::setoutpost(sysio::slug_name code, outpost_addrs outpost) {
   require_priv_caller();

   chains_t tbl(get_self());
   chain_key pk{code};
   auto it = tbl.find(pk);
   sysio::check(it != tbl.end(), "sysio.chains: chain code not registered");
   sysio::check(!it->is_depot, "sysio.chains: the depot self-row has no remote deployment");

   validate_outpost_addrs(it->kind, outpost);

   tbl.modify(ram_payer, pk, [&](auto& row) {
      row.outpost = std::move(outpost);
   });
}

} // namespace sysio
