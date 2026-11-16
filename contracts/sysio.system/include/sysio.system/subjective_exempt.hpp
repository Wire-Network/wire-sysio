#pragma once

#include <sysio.system/subjective_exempt_constants.hpp>

#include <sysio/action.hpp>
#include <sysio/contract.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/name.hpp>

namespace sysiosystem {

using sysio::name;

/// Well-known sysio.bond account, which lists an underwriter when it first bonds.
namespace bond_refs {
   constexpr name account = "sysio.bond"_n;
}

// -------------------------------------------------------------------------------------------------
/// Key of `subjexempt`.
struct subjexempt_key {
   name account;
   SYSLIB_SERIALIZE(subjexempt_key, (account))
};

/// An account that every node exempts from subjective CPU billing.
struct [[sysio::table("subjexempt"), sysio::contract("sysio.system")]] subj_exempt {
   name account;

   SYSLIB_SERIALIZE(subj_exempt, (account))
};

// -------------------------------------------------------------------------------------------------
using subj_exempt_table = sysio::kv::table<"subjexempt"_n, subjexempt_key, subj_exempt>;

static_assert(name{subjective_exemption::table_exempt} == "subjexempt"_n,
              "the host reads the table under the shared name");

// -------------------------------------------------------------------------------------------------
/**
 * The accounts every node exempts from subjective CPU billing, beside the active batch and underwriter operators it
 * reads from sysio.opreg. A node reloads the list on irreversible blocks, so a change takes effect there about a
 * minute later.
 */
struct [[sysio::contract("sysio.system")]] subjective_exempt : public sysio::contract {

   subjective_exempt(name s, name code, sysio::datastream<const char*> ds)
      : sysio::contract(s, code, ds) {}

   /**
    * Adds an account to the list. Does nothing if it is listed already.
    * Requires the authority of this contract, or of sysio.bond.
    * @param account The account to exempt. Must exist.
    */
   [[sysio::action]]
   void addsubjex(name account);

   /**
    * Removes an account from the list. Requires the authority of this contract.
    * @param account The account to stop exempting. Must be listed.
    */
   [[sysio::action]]
   void delsubjex(name account);

   using addsubjex_action = sysio::action_wrapper<"addsubjex"_n, &subjective_exempt::addsubjex>;
};

} // namespace sysiosystem
