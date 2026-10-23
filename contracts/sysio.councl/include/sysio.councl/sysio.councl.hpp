#pragma once

/**
 * @file sysio.councl.hpp
 * @brief Council election contract — fills 21 council seats through simultaneous flights and
 *        shared voting rounds across all frozen owner tiers. See DESIGN.md for the full model.
 */

#include <sysio/crypto.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/kv_scoped_table.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/sysio.hpp>
#include <sysio/system.hpp>

#include <sysio.councl/council_math.hpp>

#include <string>
#include <vector>

namespace sysio {

/// Contract-wide constants and identifiers for sysio.councl.
namespace councl {
inline constexpr uint8_t SEATS = 21;                             ///< tier-1 owners == council seats
inline constexpr uint8_t NO_SEAT = SEATS;                        ///< unreserved candidate position
inline constexpr uint32_t MAX_BATCH_ROWS = 1000;                 ///< snapshot/cleanup transaction limit
inline constexpr uint8_t SLATE_SIZE = 3;                         ///< candidates per repcandidate
inline constexpr uint8_t MIN_CANDIDATES = SEATS + 2;             ///< 23: <=20 elected before the last seat, +3
inline constexpr uint32_t MAX_CANDIDATES = 1000;                 ///< hard bound on one generation's candidate pool
inline constexpr size_t MAX_HANDLE_LEN = 32;                     ///< candidate-handle byte cap
inline constexpr uint64_t MAX_TIME_SLOT_SEC = 30 * 24 * 60 * 60; ///< thirty-day operational safety cap

constexpr name ROA_ACCOUNT = "sysio.roa"_n; ///< owner of the nodeowners / roastate tables
constexpr name SYSTEM_ACCOUNT = "sysio"_n;  ///< system RAM pool payer

/// Lifecycle phase of the shared election round.
enum class election_phase : uint8_t {
   NOMINATING = 0, ///< vacant T1 owners may replace their flights
   GENERATING = 1, ///< missing flights generated in seat order from a frozen seed
   VOTING = 2,     ///< immutable flights share one inclusive voting deadline
   TABULATING = 3, ///< final tallies processed in seat order
   CONTINUING = 4, ///< next crank opens a fresh round for remaining vacancies
   DONE = 5        ///< all 21 seats filled
};

/// Lifecycle phase of election initialization and generation cleanup.
enum class init_phase : uint8_t {
   REG = 0,     ///< candidate registration is open
   LOADING = 1, ///< tier snapshots are being loaded
   READY = 2,   ///< election is running or complete
   CLEANING = 3 ///< prior-generation ephemeral rows are being purged
};

/// Why the current cleanup was started; determines which generation data is retained.
enum class cleanup_mode : uint8_t {
   NONE = 0,         ///< no cleanup is active
   INIT_ABORT = 1,   ///< discard only staged snapshots and preserve candidate registration
   ACTIVE_ABORT = 2, ///< discard an unfinished election, including partial council results
   COMPLETED = 3     ///< retire a completed generation while retaining council history
};

/// Frozen voting tier, with stable values matching the ROA owner tiers.
enum class election_tier : uint8_t { T1 = 1, T2 = 2, T3 = 3 };

/// Ordered cleanup stages used by the batched `purge` action.
enum class cleanup_stage : uint8_t {
   CANDIDATES = 0,
   ROSTER = 1,
   TIER2 = 2,
   TIER3 = 3,
   FLIGHTS = 4,
   BALLOTS = 5,
   COUNCIL = 6,
   COMPLETE = 7
};

static_assert(SLATE_SIZE == 3, "the fixed action and state schema require a three-candidate slate");
} // namespace councl

/**
 * @brief The council election contract.
 *
 * Actions fall into three groups: registration (`addcandidate`/`rmcandidate`), staged init
 * (`startinit`/`loadtier`/`finalizeinit`, plus `reset`/`purge`), and the election
 * (`repcandidate`/`vote`/`settle`). `stir` is a public,
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

   /// Inspect at most `max_rows` roa owner rows while appending tier-`tier` (2 or 3) owners into
   /// the frozen snapshot. The persistent source cursor makes both reads and writes bounded;
   /// call repeatedly until that tier's scan-complete flag is set. Governance only.
   [[sysio::action]]
   void loadtier(uint8_t tier, uint32_t max_rows);

   /// Finalize init: verify the tier-2/3 snapshots and open simultaneous nominations. Governance only.
   [[sysio::action]]
   void finalizeinit();

   /// Abort LOADING or any READY election and enter staged cleanup. A LOADING abort preserves the
   /// candidate registry and generation; READY cleanup advances the generation. Governance only.
   [[sysio::action]]
   void reset();

   /// Delete up to `max_rows` mode-specific cleanup rows and finish reset once empty. Completed
   /// council results are retained; partial results from an active abort are deleted.
   [[sysio::action]]
   void purge(uint32_t max_rows);

   // ---- Election -----------------------------------------------------------

   /// Submit or atomically replace the owner's three ordered candidates in this election round.
   [[sysio::action]]
   void repcandidate(name proposer, name c1, name c2, name c3, uint64_t election_gen, uint64_t round_id);

   /// One occurrence's independent public YES/NO decisions, identified by its stable seat.
   struct flight_vote {
      uint8_t seat;
      bool v1 = false, v2 = false, v3 = false;
      SYSLIB_SERIALIZE(flight_vote, (seat)(v1)(v2)(v3))
   };

   /// Cast one immutable ballot covering every eligible flight in ascending seat order.
   /// Required identities bind the signature to the frozen candidates, election, and round.
   /// Every accepted ballot contributes once to its tier's round-wide threshold denominator,
   /// including a T1 ballot whose only remaining flight is its excluded own seat.
   [[sysio::action]]
   void vote(name voter, uint64_t election_gen, uint64_t round_id, checksum256 flight_hash,
             std::vector<flight_vote> votes);

   /// Authenticated public crank. Process at most max_steps seats (1..21), in cursor order.
   /// Generation and tabulation have separate transitions; retries cannot reopen old rounds.
   [[sysio::action]]
   void settle(name caller, uint64_t election_gen, uint64_t round_id, uint32_t max_steps);

   /// Authenticated entropy contribution. Never alters a seed already frozen for generation.
   [[sysio::action]]
   void stir(name caller);

   // -----------------------------------------------------------------------
   //  Tables
   // -----------------------------------------------------------------------

   /// Contract configuration + init progress singleton.
   struct [[sysio::table("config")]] config_state {
      councl::init_phase init_phase = councl::init_phase::REG;
      uint64_t time_slot_sec = 0;
      uint8_t network_gen = 0;       ///< roa network generation captured at startinit
      uint64_t election_gen = 0;     ///< scope for all per-election tables
      uint32_t n2 = 0;               ///< tier-2 snapshot size (set at finalize)
      uint32_t n3 = 0;               ///< tier-3 snapshot size
      uint32_t t2_loaded = 0;        ///< tier-2 loaded-row count and next snapshot index
      uint32_t t3_loaded = 0;        ///< tier-3 loaded-row count and next snapshot index
      uint64_t t2_cursor = 0;        ///< last roa primary owner inspected by the tier-2 scan
      uint64_t t3_cursor = 0;        ///< last roa primary owner inspected by the tier-3 scan
      bool t2_scan_complete = false; ///< tier-2 scan reached the end of the roa owner scope
      bool t3_scan_complete = false; ///< tier-3 scan reached the end of the roa owner scope
      uint32_t cand_count = 0;       ///< registered candidates (current generation)
      councl::cleanup_mode cleanup_mode = councl::cleanup_mode::NONE;
      councl::cleanup_stage cleanup_stage = councl::cleanup_stage::COMPLETE;

      SYSLIB_SERIALIZE(
         config_state,
         (init_phase)(time_slot_sec)(network_gen)(election_gen)(n2)(n3)(t2_loaded)(t3_loaded)(t2_cursor)(t3_cursor)(t2_scan_complete)(t3_scan_complete)(cand_count)(cleanup_mode)(cleanup_stage))
   };
   using config_t = sysio::kv::global<"config"_n, config_state>;

   /// Bounded phase/progress state. Round identities remain fresh while winners are retained.
   struct [[sysio::table("state")]] election_state {
      councl::election_phase phase = councl::election_phase::NOMINATING;
      uint64_t round_id = 0;
      time_point round_open_ts{};
      time_point vote_deadline{};
      uint8_t cursor = 0; ///< next original seat for generation or tabulation
      uint8_t seats_filled = 0;
      uint32_t t1_ballots = 0;   ///< accepted T1 ballots this round, including own-flight exclusions
      uint32_t t2_ballots = 0;   ///< accepted T2 ballots this round
      uint32_t t3_ballots = 0;   ///< accepted T3 ballots this round
      checksum256 flight_hash{}; ///< canonical generation/round/ordered-flight commitment
      checksum256 round_seed{};  ///< frozen once on entry to GENERATING
      checksum256 acc{};
      uint64_t stir_count = 0;
      SYSLIB_SERIALIZE(
         election_state,
         (phase)(round_id)(round_open_ts)(vote_deadline)(cursor)(seats_filled)(t1_ballots)(t2_ballots)(t3_ballots)(flight_hash)(round_seed)(acc)(stir_count))
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
      uint64_t claim_round = 0; ///< older claims are logically released when a new round opens
      std::vector<uint8_t> positions = {councl::NO_SEAT, councl::NO_SEAT, councl::NO_SEAT};
      SYSLIB_SERIALIZE(candidate_row, (account)(handle)(elected)(claim_round)(positions))
   };
   using candidates_t = sysio::kv::scoped_table<"candidates"_n, cand_key, candidate_row>;

   /// Public counts for one tier in one flight; votes_cast is an audit count, not the denominator.
   struct tier_tally {
      uint32_t votes_cast = 0;
      uint32_t yes1 = 0, yes2 = 0, yes3 = 0;
      SYSLIB_SERIALIZE(tier_tally, (votes_cast)(yes1)(yes2)(yes3))
   };

   /// Current/latest flight at a stable seat. Old-round rows are ignored and overwritten.
   struct [[sysio::table("flights")]] flight_row {
      uint64_t seat;
      uint64_t round_id;
      std::vector<name> candidates;
      bool automatic = false;
      std::vector<tier_tally> tallies = std::vector<tier_tally>(3);
      SYSLIB_SERIALIZE(flight_row, (seat)(round_id)(candidates)(automatic)(tallies))
   };
   using flights_t = sysio::kv::scoped_table<"flights"_n, index_key, flight_row>;

   /// Public ballot, immutable within its round; overwritten only by this voter in a later round.
   struct [[sysio::table("ballots")]] ballot_row {
      name voter;
      uint64_t round_id;
      councl::election_tier tier;
      checksum256 flight_hash;
      std::vector<flight_vote> votes;
      SYSLIB_SERIALIZE(ballot_row, (voter)(round_id)(tier)(flight_hash)(votes))
   };
   using ballots_t = sysio::kv::scoped_table<"ballots"_n, cand_key, ballot_row>;

   /// A filled council seat (the 21 outputs). Scoped by generation.
   struct [[sysio::table("council")]] council_row {
      uint64_t seat;
      name seat_owner;                   ///< roster[seat] — the tier-1 owner of this seat
      councl::election_tier filled_tier; ///< tier whose final YES tally filled this seat
      name proposer;                     ///< frozen T1 seat owner
      name member;                       ///< the elected candidate
      uint64_t round_id;                 ///< round in which the seat was filled
      SYSLIB_SERIALIZE(council_row, (seat)(seat_owner)(filled_tier)(proposer)(member)(round_id))
   };
   using council_t = sysio::kv::scoped_table<"council"_n, index_key, council_row>;

private:
   /// Reject stale signed requests before any state mutation.
   void check_round(const config_state& cfg, const election_state& st, uint64_t generation, uint64_t round) const;
   /// Mix an authenticated action tag, actor, and monotonic stir count into the accumulator.
   void do_stir(election_state& st, name action_tag, name actor);
   /// Open nominations for every vacancy and reset tier ballot counts, preserving candidates,
   /// frozen snapshots, and winners. Old-round flights, tallies, and claims become inactive.
   void open_round(election_state& st);
   /// Atomically replace one flight and its candidate-position reservations.
   void save_flight(const config_state& cfg, const election_state& st, uint8_t seat,
                    const std::vector<name>& candidates, bool automatic);
   /// Release current claims and remove a mutable flight during atomic replacement.
   void release_flight(const config_state& cfg, const election_state& st, uint8_t seat);
   /// Generate missing flights in original seat order with a fixed per-round seed.
   void generate_flights(election_state& st, const config_state& cfg, uint32_t max_steps);
   /// Commit all finalized flights, then open the shared voting window.
   void open_voting(election_state& st, const config_state& cfg);
   /// Process a bounded prefix of remaining seats using tier/position priority and each tier's
   /// round-wide submitted-ballot denominator, including T1 ballots omitting their own flight.
   void tabulate(election_state& st, const config_state& cfg, uint32_t max_steps);
   /// Persist a unique member/result without advancing any unrelated seat.
   void seat_member(election_state& st, const config_state& cfg, uint8_t seat, name member,
                    councl::election_tier filled_tier);
   // roa helpers
   /// Read the current ROA network generation.
   uint8_t roa_network_gen() const;
   /// Count generation-scoped ROA owners in a tier.
   uint32_t tier_count(uint8_t network_gen, councl::election_tier tier) const;

   // convenience
   /// Return the frozen tier-1 owner associated with a council seat.
   name roster_owner(const config_state& cfg, uint8_t seat) const;
   /// Return a frozen tier member's stable snapshot index, or an invalid sentinel.
   uint32_t member_index(const config_state& cfg, councl::election_tier tier, name who) const;
};

} // namespace sysio
