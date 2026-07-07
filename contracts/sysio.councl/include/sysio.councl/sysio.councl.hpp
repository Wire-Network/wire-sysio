#pragma once

/**
 * @file sysio.councl.hpp
 * @brief Council election contract — fills 21 council seats via a tier-1 → tier-2 → tier-3
 *        escalation ladder with strict-priority slate voting. See DESIGN.md for the full model.
 */

#include <sysio/sysio.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/kv_scoped_table.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/crypto.hpp>
#include <sysio/system.hpp>
#include <sysio/opp/types/types.pb.hpp>

#include <sysio.councl/council_math.hpp>

#include <string>
#include <vector>

namespace sysio {

/// Contract-wide constants and identifiers for sysio.councl.
namespace councl {
   inline constexpr uint8_t  SEATS          = 21;       ///< tier-1 owners == council seats
   inline constexpr uint8_t  T1_VOTERS      = 20;       ///< SEATS - 1 (seat owner never votes)
   inline constexpr uint8_t  SLATE_SIZE     = 3;        ///< candidates per repcandidate
   inline constexpr uint8_t  MIN_CANDIDATES = SEATS + 2;///< 23: <=20 elected before the last seat, +3
   inline constexpr size_t   MAX_HANDLE_LEN = 32;       ///< twitter-handle byte cap

   constexpr name ROA_ACCOUNT    = "sysio.roa"_n;       ///< owner of the nodeowners / roastate tables
   constexpr name SYSTEM_ACCOUNT = "sysio"_n;           ///< owner of the nodecount table; RAM pool payer

   // Election phase (state.phase). uint8 for a stable ABI; named to avoid magic literals.
   inline constexpr uint8_t PH_AWAIT_REP = 0; ///< waiting for the active proposer's repcandidate
   inline constexpr uint8_t PH_VOTING    = 1; ///< a slate is open for voting
   inline constexpr uint8_t PH_BACKSTOP  = 2; ///< tier-3 exhausted; awaiting governance forceassign
   inline constexpr uint8_t PH_DONE      = 3; ///< all 21 seats filled

   // Init phase (config.init_phase).
   inline constexpr uint8_t IP_REG     = 0; ///< candidate registration open
   inline constexpr uint8_t IP_LOADING = 1; ///< tier-2/3 snapshots being loaded
   inline constexpr uint8_t IP_READY   = 2; ///< election running / finished

   // filled_tier sentinel for a governance-assigned seat.
   inline constexpr uint8_t TIER_GOV = 0;
}

/**
 * @brief The council election contract.
 *
 * Actions fall into three groups: registration (`addcandidate`/`rmcandidate`), staged init
 * (`startinit`/`loadtier`/`finalizeinit`, plus `reset`), and the election
 * (`repcandidate`/`vote`/`settle`/`forceassign`). `stir` is a permissionless entropy crank.
 */
class [[sysio::contract("sysio.councl")]] council : public contract {
public:
   using contract::contract;

   // ---- Registration -------------------------------------------------------

   /// Self-register as a council candidate. `handle` is a short (<= MAX_HANDLE_LEN) label
   /// (assumed a twitter handle for now). Allowed only while registration is open.
   [[sysio::action]]
   void addcandidate(name account, std::string handle);

   /// Remove a candidate before the election starts. Governance only.
   [[sysio::action]]
   void rmcandidate(name account);

   // ---- Staged initialization ---------------------------------------------

   /// Begin an election: freeze the ordered tier-1 roster and close registration. `ordered_owners`
   /// must be a permutation of exactly the 21 roa tier-1 node owners. Governance only.
   [[sysio::action]]
   void startinit(uint64_t time_slot_sec, std::vector<name> ordered_owners);

   /// Append up to `max_rows` of roa's tier-`tier` (2 or 3) owners into the frozen snapshot,
   /// resuming from the load cursor. Call repeatedly until complete. Governance only.
   [[sysio::action]]
   void loadtier(uint8_t tier, uint32_t max_rows);

   /// Finalize init: verify the tier-2/3 snapshots are complete and open seat 0. Governance only.
   [[sysio::action]]
   void finalizeinit();

   /// Start a fresh election generation (after DONE): bump the generation and reopen registration.
   /// Governance only. Prior-generation rows remain under their old scope and are ignored.
   [[sysio::action]]
   void reset();

   // ---- Election -----------------------------------------------------------

   /// The active proposer nominates a slate of 3 distinct, un-elected candidates.
   [[sysio::action]]
   void repcandidate(name proposer, name c1, name c2, name c3);

   /// Cast an independent yes/no on each of the 3 current-slate candidates. One ballot per voter
   /// per attempt; the proposer never votes on their own slate.
   [[sysio::action]]
   void vote(name voter, bool v1, bool v2, bool v3);

   /// Permissionless crank: push a timed-out attempt forward and stir entropy.
   [[sysio::action]]
   void settle();

   /// Governance backstop: seat an un-elected candidate when tier-3 is exhausted (phase BACKSTOP).
   [[sysio::action]]
   void forceassign(name member);

   /// Permissionless entropy crank: advance the accumulator (Variant B — no block number folded).
   [[sysio::action]]
   void stir();

   // -----------------------------------------------------------------------
   //  Tables
   // -----------------------------------------------------------------------

   /// Contract configuration + init progress singleton.
   struct [[sysio::table("config")]] config_state {
      bool     initialized  = false;
      uint8_t  init_phase   = councl::IP_REG;
      bool     reg_open     = true;
      uint64_t time_slot_sec = 0;
      uint8_t  network_gen  = 0;  ///< roa network generation captured at startinit
      uint64_t election_gen = 0;  ///< scope for all per-election tables
      uint32_t n2           = 0;  ///< tier-2 snapshot size (set at finalize)
      uint32_t n3           = 0;  ///< tier-3 snapshot size
      uint32_t t2_loaded    = 0;  ///< tier-2 load cursor
      uint32_t t3_loaded    = 0;  ///< tier-3 load cursor
      uint32_t cand_count   = 0;  ///< registered candidates (current generation)

      SYSLIB_SERIALIZE(config_state,
         (initialized)(init_phase)(reg_open)(time_slot_sec)(network_gen)
         (election_gen)(n2)(n3)(t2_loaded)(t3_loaded)(cand_count))
   };
   using config_t = sysio::kv::global<"config"_n, config_state>;

   /// Live election cursor + current round tallies + entropy accumulator.
   struct [[sysio::table("state")]] election_state {
      uint8_t    phase          = councl::PH_AWAIT_REP;
      uint8_t    active_seat     = 0;   ///< 0..20
      uint8_t    tier            = 1;   ///< 1, 2, or 3
      name       proposer{};
      uint64_t   round_id        = 0;   ///< monotonic attempt counter (also selection nonce)
      time_point round_open_ts{};       ///< when the current attempt opened (propose-deadline base)
      time_point vote_deadline{};
      uint64_t   elect_N         = 0;   ///< electorate size of the current round
      uint32_t   eligible_voters = 0;   ///< voters expected this round (excludes the proposer)
      uint32_t   votes_cast      = 0;
      uint32_t   tried_count     = 0;   ///< tier-3 accounts tried for the current seat
      uint8_t    seats_filled    = 0;
      // current slate + independent per-candidate tallies
      name       c1{}, c2{}, c3{};
      uint64_t   yes1 = 0, yes2 = 0, yes3 = 0;
      uint64_t   no1  = 0, no2  = 0, no3  = 0;
      // entropy accumulator (Variant B)
      checksum256 acc{};
      uint64_t    stir_count = 0;

      SYSLIB_SERIALIZE(election_state,
         (phase)(active_seat)(tier)(proposer)(round_id)(round_open_ts)(vote_deadline)
         (elect_N)(eligible_voters)(votes_cast)(tried_count)(seats_filled)
         (c1)(c2)(c3)(yes1)(yes2)(yes3)(no1)(no2)(no3)(acc)(stir_count))
   };
   using state_t = sysio::kv::global<"state"_n, election_state>;

   /// Ordered-index key shared by the roster / tier snapshots and the council output.
   struct index_key {
      uint64_t idx;
      uint64_t primary_key() const { return idx; }
      SYSLIB_SERIALIZE(index_key, (idx))
   };

   // One frozen node-owner slot, ordered by `idx`, with a by-owner secondary index for membership
   // checks. The three tiers use separate row structs (identical shape) so each carries a
   // [[sysio::table]] attribute matching its KV table name — the ABI convention this repo follows
   // (one value struct per table name). The shape is shared via the SYSLIB_SERIALIZE field list.
   struct [[sysio::table("roster")]] roster_row {
      uint64_t idx;
      name     owner;
      uint64_t by_owner() const { return owner.value; }
      SYSLIB_SERIALIZE(roster_row, (idx)(owner))
   };
   struct [[sysio::table("tier2")]] tier2_row {
      uint64_t idx;
      name     owner;
      uint64_t by_owner() const { return owner.value; }
      SYSLIB_SERIALIZE(tier2_row, (idx)(owner))
   };
   struct [[sysio::table("tier3")]] tier3_row {
      uint64_t idx;
      name     owner;
      uint64_t by_owner() const { return owner.value; }
      SYSLIB_SERIALIZE(tier3_row, (idx)(owner))
   };
   using roster_t = sysio::kv::scoped_table<"roster"_n, index_key, roster_row,
      sysio::kv::index<"byowner"_n, sysio::const_mem_fun<roster_row, uint64_t, &roster_row::by_owner>>>;
   using tier2_t = sysio::kv::scoped_table<"tier2"_n, index_key, tier2_row,
      sysio::kv::index<"byowner"_n, sysio::const_mem_fun<tier2_row, uint64_t, &tier2_row::by_owner>>>;
   using tier3_t = sysio::kv::scoped_table<"tier3"_n, index_key, tier3_row,
      sysio::kv::index<"byowner"_n, sysio::const_mem_fun<tier3_row, uint64_t, &tier3_row::by_owner>>>;

   /// A registered candidate.
   struct cand_key {
      uint64_t account;
      uint64_t primary_key() const { return account; }
      SYSLIB_SERIALIZE(cand_key, (account))
   };
   struct [[sysio::table("candidates")]] candidate_row {
      name        account;
      std::string handle;
      bool        elected = false;
      SYSLIB_SERIALIZE(candidate_row, (account)(handle)(elected))
   };
   using candidates_t = sysio::kv::scoped_table<"candidates"_n, cand_key, candidate_row>;

   /// One voter's ballot in an attempt. Scoped by (generation, round_id); existence = has-voted.
   struct voter_key {
      uint64_t voter;
      uint64_t primary_key() const { return voter; }
      SYSLIB_SERIALIZE(voter_key, (voter))
   };
   struct [[sysio::table("ballots")]] ballot_row {
      name voter;
      bool v1, v2, v3;
      SYSLIB_SERIALIZE(ballot_row, (voter)(v1)(v2)(v3))
   };
   using ballots_t = sysio::kv::scoped_table<"ballots"_n, voter_key, ballot_row>;

   /// A tier-3 index already attempted for the current seat. Scoped by (generation, seat).
   struct [[sysio::table("tried3")]] tried_row {
      uint64_t idx;
      SYSLIB_SERIALIZE(tried_row, (idx))
   };
   using tried3_t = sysio::kv::scoped_table<"tried3"_n, index_key, tried_row>;

   /// A filled council seat (the 21 outputs). Scoped by generation.
   struct [[sysio::table("council")]] council_row {
      uint64_t seat;
      name     seat_owner;   ///< roster[seat] — the tier-1 owner of this seat
      uint8_t  filled_tier;  ///< 1/2/3, or TIER_GOV for a governance assignment
      name     proposer;     ///< the account whose slate won
      name     member;       ///< the elected candidate
      SYSLIB_SERIALIZE(council_row, (seat)(seat_owner)(filled_tier)(proposer)(member))
   };
   using council_t = sysio::kv::scoped_table<"council"_n, index_key, council_row>;

private:
   using NodeOwnerTier = opp::types::NodeOwnerTier;

   // Per-election table scope is the generation; per-round/seat tables fold the generation in.
   static uint64_t gr_scope(uint64_t gen, uint64_t x) { return (gen << 40) | (x & ((uint64_t(1) << 40) - 1)); }

   // Entropy + selection
   void        do_stir(election_state& st, name action_tag, name actor);
   name        select_from_tier(election_state& st, uint8_t tier, uint32_t avail,
                                const config_state& cfg);

   // State machine
   void settle_if_elapsed(election_state& st, const config_state& cfg);
   void try_resolve(election_state& st, const config_state& cfg);
   void win_attempt(election_state& st, const config_state& cfg, name winner);
   void fail_attempt(election_state& st, const config_state& cfg);
   void open_tier_attempt(election_state& st, const config_state& cfg, uint8_t tier, name proposer);
   void advance_seat(election_state& st, const config_state& cfg);

   // roa / nodecount helpers
   uint8_t     roa_network_gen() const;
   uint32_t    tier_count(uint8_t tier) const; // from sysio.system::nodecount

   // convenience
   name        roster_owner(const config_state& cfg, uint8_t seat) const;
   bool        is_member(const config_state& cfg, uint8_t tier, name who) const;
};

} // namespace sysio
