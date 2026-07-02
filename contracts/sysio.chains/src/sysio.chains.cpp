#include <sysio.chains/sysio.chains.hpp>
#include <sysio.epoch/sysio.epoch.hpp>

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
//  Addresses are consensus facts (every operator reads the same row), so a
//  malformed value would break relay for the whole network. Validate format
//  at the ingress boundary. Empty is allowed (not-yet-configured; the batch
//  operator fail-closed skips such rows) except where a field is structurally
//  meaningless for the kind (WIRE has no remote endpoint; an SVM outpost is a
//  single program with no separate inbound contract).
// ---------------------------------------------------------------------------

constexpr size_t EVM_ADDR_LEN = 42;    // "0x" + 20 bytes hex
constexpr size_t SVM_ADDR_MIN = 32;    // base58 of a 32-byte pubkey, lower bound
constexpr size_t SVM_ADDR_MAX = 44;    // base58 of a 32-byte pubkey, upper bound

bool is_lower_or_digit_hex(char c) {
   return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/// Bitcoin/Solana base58 alphabet — [1-9A-HJ-NP-Za-km-z] (excludes 0, O, I, l).
bool is_base58_char(char c) {
   return (c >= '1' && c <= '9')
       || (c >= 'A' && c <= 'H') || (c >= 'J' && c <= 'N') || (c >= 'P' && c <= 'Z')
       || (c >= 'a' && c <= 'k') || (c >= 'm' && c <= 'z');
}

void check_evm_addr(const std::string& addr, const char* label) {
   sysio::check(addr.size() == EVM_ADDR_LEN && addr[0] == '0' && addr[1] == 'x',
                std::string("sysio.chains: ") + label + " must be a 0x-prefixed 20-byte hex address");
   for (size_t i = 2; i < addr.size(); ++i) {
      sysio::check(is_lower_or_digit_hex(addr[i]),
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

/// Validate the (opp_addr, opp_inbound_addr) pair against the chain kind.
/// Non-empty values must match the kind's format; structurally-unused fields
/// must be empty.
void validate_outpost_addrs(opp::types::ChainKind kind,
                            const std::string& opp_addr,
                            const std::string& opp_inbound_addr) {
   switch (kind) {
      case opp::types::CHAIN_KIND_WIRE:
         sysio::check(opp_addr.empty() && opp_inbound_addr.empty(),
                      "sysio.chains: the WIRE depot self-row has no remote endpoint");
         break;
      case opp::types::CHAIN_KIND_EVM:
         if (!opp_addr.empty())         check_evm_addr(opp_addr,         "opp_addr");
         if (!opp_inbound_addr.empty()) check_evm_addr(opp_inbound_addr, "opp_inbound_addr");
         break;
      case opp::types::CHAIN_KIND_SVM:
         if (!opp_addr.empty()) check_svm_addr(opp_addr, "opp_addr");
         sysio::check(opp_inbound_addr.empty(),
                      "sysio.chains: an SVM outpost is a single program; opp_inbound_addr must be empty");
         break;
      default:
         // Future kinds: no format constraint yet.
         break;
   }
}

} // namespace

void chains::regchain(opp::types::ChainKind kind,
                      sysio::slug_name       code,
                      uint32_t              external_chain_id,
                      std::string           name,
                      std::string           description,
                      std::string           opp_addr,
                      std::string           opp_inbound_addr) {
   require_priv_caller();

   sysio::check(kind != opp::types::CHAIN_KIND_UNKNOWN,
                "sysio.chains: kind must not be UNKNOWN");

   validate_outpost_addrs(kind, opp_addr, opp_inbound_addr);

   chains_t tbl(get_self());
   chain_key pk{code};
   sysio::check(tbl.find(pk) == tbl.end(),
                "sysio.chains: chain code already registered");

   // Enforce: at most one row with kind == WIRE (the depot self-row).
   if (kind == opp::types::CHAIN_KIND_WIRE) {
      auto by_kind_idx = tbl.template get_index<"bykind"_n>();
      const auto wire_kind_value = magic_enum::enum_integer(opp::types::CHAIN_KIND_WIRE);
      sysio::check(by_kind_idx.lower_bound(wire_kind_value) == by_kind_idx.upper_bound(wire_kind_value),
                   "sysio.chains: a WIRE chain (depot self-row) already exists");
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
      .opp_addr           = std::move(opp_addr),
      .opp_inbound_addr   = std::move(opp_inbound_addr),
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

void chains::setoutpost(sysio::slug_name code,
                        std::string      opp_addr,
                        std::string      opp_inbound_addr) {
   require_priv_caller();

   chains_t tbl(get_self());
   chain_key pk{code};
   auto it = tbl.find(pk);
   sysio::check(it != tbl.end(), "sysio.chains: chain code not registered");
   sysio::check(!it->is_depot, "sysio.chains: the WIRE depot self-row has no remote endpoint");

   validate_outpost_addrs(it->kind, opp_addr, opp_inbound_addr);

   tbl.modify(ram_payer, pk, [&](auto& row) {
      row.opp_addr         = std::move(opp_addr);
      row.opp_inbound_addr = std::move(opp_inbound_addr);
   });
}

} // namespace sysio
