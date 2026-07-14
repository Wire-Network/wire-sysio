#pragma once

/**
 * @file sysio.councl.hpp
 * @brief Council election contract — fills 21 council seats via a tier-1 → tier-2 → tier-3
 *        escalation ladder with strict-priority slate voting. See DESIGN.md for the full model.
 */

#include <string>
#include <sysio.councl/council_math.hpp>
#include <sysio/crypto.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/kv_scoped_table.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/sysio.hpp>
#include <sysio/system.hpp>
#include <vector>

namespace sysio {

/// Contract-wide constants and identifiers for sysio.councl.
namespace councl {
inline constexpr uint8_t SEATS = 21;                             ///< tier-1 owners == council seats
inline constexpr uint8_t T1_VOTERS = 20;                         ///< SEATS - 1 (seat owner never votes)
inline constexpr uint8_t SLATE_SIZE = 3;                         ///< candidates per repcandidate
inline constexpr uint8_t MIN_CANDIDATES = SEATS + 2;             ///< 23: <=20 elected before the last seat, +3
inline constexpr uint32_t MAX_CANDIDATES = 1000;                 ///< hard bound on one generation's candidate pool
inline constexpr size_t MAX_HANDLE_LEN = 32;                     ///< candidate-handle byte cap
inline constexpr uint64_t MAX_TIME_SLOT_SEC = 30 * 24 * 60 * 60; ///< thirty-day operational safety cap

constexpr name ROA_ACCOUNT = "sysio.roa"_n; ///< owner of the nodeowners / roastate tables
constexpr name SYSTEM_ACCOUNT = "sysio"_n;  ///< owner of the nodecount table; RAM pool payer

/// Lifecycle phase of the active election attempt.
enum class election_phase : uint8_t {
   AWAIT_REP = 0, ///< waiting for the active proposer's nomination
   VOTING = 1,    ///< a slate is open for voting
   BACKSTOP = 2,  ///< awaiting governance assignment
   DONE = 3       ///< all seats are filled
};

/// Lifecycle phase of election initialization and generation cleanup.
enum class init_phase : uint8_t {
   REG = 0,     ///< candidate registration is open
   LOADING = 1, ///< tier snapshots are being loaded
   READY = 2,   ///< election is running or complete
   CLEANING = 3 ///< prior-generation ephemeral rows are being purged
};

/// Tier responsible for an attempt or completed seat.
enum class election_tier : uint8_t { GOVERNANCE = 0, T1 = 1, T2 = 2, T3 = 3 };

/// Ordered cleanup stages used by the batched `purge` action.
enum class cleanup_stage : uint8_t { CANDIDATES = 0, ROSTER = 1, TIER2 = 2, TIER3 = 3, REMAP = 4, COMPLETE = 5 };

static_assert(T1_VOTERS == SEATS - 1, "tier-1 electorate must exclude exactly the seat owner");
} // namespace councl

/**
 * @brief The council election contract.
 *
 * Actions fall into three groups: registration (`addcandidate`/`rmcandidate`), staged init
 * (`startinit`/`loadtier`/`finalizeinit`, plus `reset`/`purge`), and the election
 * (`repcandidate`/`vote`/`settle`/`forceback`/`forceassign`). `stir` is a public,
 * caller-authenticated entropy crank.
 */
class [[sysio::contract("sysio.councl")]] council : public contract {
public:
   using contract::contract;

   // ---- Registration -------------------------------------------------------

   /// Self-register as a council candidate and pay the row RAM. `handle` is a 1..32-byte label
   /// restricted to ASCII alphanumeric characters plus `@`, `_`, `-`, and `.`.
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
   /// skipping owners already snapshotted (identity-based resume, so owners forcereg'd in roa
   /// mid-load are absorbed by a later batch). Call repeatedly until `finalizeinit`'s count
   /// cross-check passes. Idempotent. Governance only.
   [[sysio::action]]
   void loadtier(uint8_t tier, uint32_t max_rows);

   /// Finalize init: verify the tier-2/3 snapshots are complete and open seat 0. Governance only.
   [[sysio::action]]
   void finalizeinit();

   /// After DONE, enter staged cleanup for the completed generation. Governance only. Call
   /// `purge` until cleanup advances the generation and reopens registration.
   [[sysio::action]]
   void reset();

   /// Delete up to `max_rows` prior-generation ephemeral rows and finish reset once empty.
   /// Council results are deliberately retained as the permanent election history.
   [[sysio::action]]
   void purge(uint32_t max_rows);

   // ---- Election -----------------------------------------------------------

   /// The active proposer nominates a slate of 3 distinct, un-elected candidates.
   [[sysio::action]]
   void repcandidate(name proposer, name c1, name c2, name c3);

   /// Cast an independent yes/no on each of the 3 current-slate candidates. One vote per voter
   /// per attempt is enforced by a compact bitmap; the proposer never votes on their own slate.
   [[sysio::action]]
   void vote(name voter, bool v1, bool v2, bool v3);

   /// Public caller-authenticated crank: push a timed-out attempt forward and stir entropy.
   [[sysio::action]]
   void settle(name caller);

   /// Governance recovery: move an elapsed active attempt directly to BACKSTOP.
   [[sysio::action]]
   void forceback();

   /// Governance backstop: seat an un-elected candidate when tier-3 is exhausted (phase BACKSTOP).
   [[sysio::action]]
   void forceassign(name member);

   /// Public caller-authenticated entropy crank; also advances elapsed election state.
   [[sysio::action]]
   void stir(name caller);

   // -----------------------------------------------------------------------
   //  Tables
   // -----------------------------------------------------------------------

   /// Contract configuration + init progress singleton.
   struct [[sysio::table("config")]] config_state {
      councl::init_phase init_phase = councl::init_phase::REG;
      uint64_t time_slot_sec = 0;
      uint8_t network_gen = 0;   ///< roa network generation captured at startinit
      uint64_t election_gen = 0; ///< scope for all per-election tables
      uint32_t n2 = 0;           ///< tier-2 snapshot size (set at finalize)
      uint32_t n3 = 0;           ///< tier-3 snapshot size
      uint32_t t2_loaded = 0;    ///< tier-2 loaded-row count and next snapshot index
      uint32_t t3_loaded = 0;    ///< tier-3 loaded-row count and next snapshot index
      uint32_t cand_count = 0;   ///< registered candidates (current generation)
      councl::cleanup_stage cleanup_stage = councl::cleanup_stage::COMPLETE;
      uint8_t cleanup_seat = 0; ///< remap scope currently being purged

      SYSLIB_SERIALIZE(
         config_state,
         (init_phase)(time_slot_sec)(network_gen)(election_gen)(n2)(n3)(t2_loaded)(t3_loaded)(cand_count)(cleanup_stage)(cleanup_seat))
   };
   using config_t = sysio::kv::global<"config"_n, config_state>;

   /// Live election cursor + current round tallies + entropy accumulator.
   struct [[sysio::table("state")]] election_state {
      councl::election_phase phase = councl::election_phase::AWAIT_REP;
      uint8_t active_seat = 0; ///< 0..20
      councl::election_tier tier = councl::election_tier::T1;
      name proposer{};
      uint64_t round_id = 0;      ///< monotonic attempt counter (also selection nonce)
      time_point round_open_ts{}; ///< when the current attempt opened (propose-deadline base)
      time_point vote_deadline{};
      uint32_t elect_N = 0;         ///< electorate size of the current round
      uint32_t eligible_voters = 0; ///< voters expected this round (excludes the proposer)
      uint32_t votes_cast = 0;
      uint32_t tier3_available = 0; ///< untried tier-3 proposers for the current seat
      uint8_t seats_filled = 0;
      std::vector<uint8_t> voted_bitmap; ///< one bit per frozen tier member; prevents duplicate votes
      // current slate + independent per-candidate tallies
      name c1{}, c2{}, c3{};
      uint32_t yes1 = 0, yes2 = 0, yes3 = 0;
      uint32_t no1 = 0, no2 = 0, no3 = 0;
      // entropy accumulator (Variant B)
      checksum256 acc{};
      uint64_t stir_count = 0;

      SYSLIB_SERIALIZE(
         election_state,
         (phase)(active_seat)(tier)(proposer)(round_id)(round_open_ts)(vote_deadline)(elect_N)(eligible_voters)(votes_cast)(tier3_available)(seats_filled)(voted_bitmap)(c1)(c2)(c3)(yes1)(yes2)(yes3)(no1)(no2)(no3)(acc)(stir_count))
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
      name owner;
      uint64_t by_owner() const { return owner.value; }
      SYSLIB_SERIALIZE(roster_row, (idx)(owner))
   };
   struct [[sysio::table("tier2")]] tier2_row {
      uint64_t idx;
      name owner;
      uint64_t by_owner() const { return owner.value; }
      SYSLIB_SERIALIZE(tier2_row, (idx)(owner))
   };
   struct [[sysio::table("tier3")]] tier3_row {
      uint64_t idx;
      name owner;
      uint64_t by_owner() const { return owner.value; }
      SYSLIB_SERIALIZE(tier3_row, (idx)(owner))
   };
   using roster_t = sysio::kv::scoped_table<
      "roster"_n, index_key, roster_row,
      sysio::kv::index<"byowner"_n, sysio::const_mem_fun<roster_row, uint64_t, &roster_row::by_owner>>>;
   using tier2_t = sysio::kv::scoped_table<
      "tier2"_n, index_key, tier2_row,
      sysio::kv::index<"byowner"_n, sysio::const_mem_fun<tier2_row, uint64_t, &tier2_row::by_owner>>>;
   using tier3_t = sysio::kv::scoped_table<
      "tier3"_n, index_key, tier3_row,
      sysio::kv::index<"byowner"_n, sysio::const_mem_fun<tier3_row, uint64_t, &tier3_row::by_owner>>>;

   /// A registered candidate.
   struct cand_key {
      uint64_t account;
      uint64_t primary_key() const { return account; }
      SYSLIB_SERIALIZE(cand_key, (account))
   };
   struct [[sysio::table("candidates")]] candidate_row {
      name account;
      std::string handle;
      bool elected = false;
      SYSLIB_SERIALIZE(candidate_row, (account)(handle)(elected))
   };
   using candidates_t = sysio::kv::scoped_table<"candidates"_n, cand_key, candidate_row>;

   /// Fisher-Yates virtual-to-actual index remap for O(1) tier-3 selection.
   struct [[sysio::table("tier3remap")]] remap_row {
      uint64_t virtual_idx;
      uint64_t actual_idx;
      SYSLIB_SERIALIZE(remap_row, (virtual_idx)(actual_idx))
   };
   using tier3_remap_t = sysio::kv::scoped_table<"tier3remap"_n, index_key, remap_row>;

   /// A filled council seat (the 21 outputs). Scoped by generation.
   struct [[sysio::table("council")]] council_row {
      uint64_t seat;
      name seat_owner;                   ///< roster[seat] — the tier-1 owner of this seat
      councl::election_tier filled_tier; ///< tier that filled this seat, or GOVERNANCE
      name proposer;                     ///< the account whose slate won
      name member;                       ///< the elected candidate
      SYSLIB_SERIALIZE(council_row, (seat)(seat_owner)(filled_tier)(proposer)(member))
   };
   using council_t = sysio::kv::scoped_table<"council"_n, index_key, council_row>;

private:
   /// Fold a generation and bounded seat-like value into one scoped-table key.
   static uint64_t gr_scope(uint64_t gen, uint64_t x);

   // Entropy + selection
   /// Mix an authenticated action tag, actor, and monotonic stir count into the accumulator.
   void do_stir(election_state& st, name action_tag, name actor);
   /// Derive a deterministic virtual index for the current seat and round.
   uint64_t selection_index(const election_state& st, uint32_t available) const;
   /// Select the tier-2 proposer without mutating the frozen snapshot.
   name select_tier2_proposer(const election_state& st, const config_state& cfg) const;
   /// Select and remove one tier-3 proposer through the persistent Fisher-Yates remap.
   name select_tier3_proposer(election_state& st, const config_state& cfg);

   // State machine
   /// Resolve completed voting or advance an elapsed nomination/voting attempt.
   void resolve_or_settle(election_state& st, const config_state& cfg);
   /// Evaluate the current slate and apply a win or failed-attempt transition when conclusive.
   void try_resolve(election_state& st, const config_state& cfg);
   /// Persist a filled council seat and advance the election cursor.
   void seat_member(election_state& st, const config_state& cfg, name member, councl::election_tier filled_tier,
                    name proposer);
   /// Complete the current attempt with its strict-priority winner.
   void win_attempt(election_state& st, const config_state& cfg, name winner);
   /// Escalate a failed attempt or enter the governance backstop when no proposer remains.
   void fail_attempt(election_state& st, const config_state& cfg);
   /// Open a fresh nomination attempt for the specified tier and proposer.
   void open_tier_attempt(election_state& st, councl::election_tier tier, name proposer);
   /// Advance to the next seat, or mark the election done after the final seat.
   void advance_seat(election_state& st, const config_state& cfg);

   // roa / nodecount helpers
   /// Read the current ROA network generation.
   uint8_t roa_network_gen() const;
   /// Read a tier owner count from `sysio.system::nodecount`.
   uint32_t tier_count(councl::election_tier tier) const;

   // convenience
   /// Return the frozen tier-1 owner associated with a council seat.
   name roster_owner(const config_state& cfg, uint8_t seat) const;
   /// Return a frozen tier member's stable snapshot index, or an invalid sentinel.
   uint32_t member_index(const config_state& cfg, councl::election_tier tier, name who) const;
};

} // namespace sysio
