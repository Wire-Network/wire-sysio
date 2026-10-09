#include <sysio/opp/types/types.pb.hpp>

#include <magic_enum/magic_enum.hpp>
#include <sysio.councl/sysio.councl.hpp>
#include <sysio.system/emissions.hpp> // shared node-owner caps

#include <algorithm>
#include <array>
#include <climits>
#include <limits>
#include <sysio.roa.hpp> // roa::roastate_t / roa::nodeowners_t — tier membership
#include <tuple>
#include <vector>

namespace sysio {

using councl_math::resolve_final;
using councl_math::round_result;

namespace {
// System-owned rows bill to the sysio RAM pool (privileged-contract model, as sysio.chalg
// does): the council account stays at code+abi size while row growth draws from the pool.
constexpr name RAM_PAYER = councl::SYSTEM_ACCOUNT;

// Domain-separation tag for the entropy accumulator seed at election start.
constexpr name ACC_SEED_TAG = "councilseed"_n;

constexpr name ACTION_REPCANDIDATE = "repcandi"_n; ///< shortened on-chain tag for repcandidate
constexpr name ACTION_VOTE = "vote"_n;
constexpr name ACTION_SETTLE = "settle"_n;
constexpr name ACTION_STIR = "stir"_n;

constexpr uint32_t INVALID_MEMBER_INDEX = std::numeric_limits<uint32_t>::max();

using NodeOwnerTier = opp::types::NodeOwnerTier;

static_assert(councl::SEATS == sysiosystem::emissions::T1_MAX_NODE_OWNERS,
              "council seats must match the system tier-1 owner cap");

/// Convert a council tier to its stable integer representation for ROA table comparisons.
constexpr uint8_t tier_integer(councl::election_tier tier) {
   return magic_enum::enum_integer(tier);
}

static_assert(tier_integer(councl::election_tier::T1) == magic_enum::enum_integer(NodeOwnerTier::NODE_OWNER_TIER_T1) &&
                 tier_integer(councl::election_tier::T2) ==
                    magic_enum::enum_integer(NodeOwnerTier::NODE_OWNER_TIER_T2) &&
                 tier_integer(councl::election_tier::T3) == magic_enum::enum_integer(NodeOwnerTier::NODE_OWNER_TIER_T3),
              "council and protobuf node-owner tiers must retain identical wire values");

/// Return whether a candidate handle contains only UI-safe printable handle characters.
bool valid_handle(const std::string& handle) {
   if (handle.empty() || handle.size() > councl::MAX_HANDLE_LEN)
      return false;
   for (const unsigned char ch : handle) {
      const bool alnum = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
      if (!alnum && ch != '@' && ch != '_' && ch != '-' && ch != '.')
         return false;
   }
   return true;
}

/// Result of one bounded table-erasure pass.
struct erase_result {
   uint32_t erased;
   bool drained;
};

/// Erase at most `max_rows` rows and report both the number removed and whether the table drained.
template <typename Table>
erase_result erase_rows(Table& table, uint32_t max_rows) {
   uint32_t erased = 0;
   auto it = table.begin();
   while (it != table.end() && erased < max_rows) {
      it = table.erase(std::move(it));
      ++erased;
   }
   return erase_result{erased, it == table.end()};
}

/// Return the stable index of `owner` in a typed frozen snapshot, or the invalid sentinel.
template <typename Table>
uint32_t frozen_member_index(Table& table, name owner) {
   auto by_owner = table.template get_index<"byowner"_n>();
   auto it = by_owner.find(owner.value);
   return it == by_owner.end() ? INVALID_MEMBER_INDEX : static_cast<uint32_t>(it->idx);
}

/// Result of one source-read-bounded snapshot pass.
struct snapshot_result {
   uint32_t scanned;
   uint32_t written;
   uint64_t cursor;
   bool complete;
};

/// Inspect at most `max_rows` ROA rows and append matching, not-yet-snapshotted owners.
template <typename Row, typename Table>
snapshot_result append_snapshot(Table& table, roa::nodeowners_t& owners, uint8_t raw_tier, uint32_t already,
                                uint64_t cursor, uint32_t max_rows) {
   auto by_owner = table.template get_index<"byowner"_n>();
   uint32_t scanned = 0;
   uint32_t written = 0;

   // The persistent primary-key cursor makes max_rows a real read bound. Reaching end marks this
   // pass complete. If finalize later detects a newly inserted earlier identity, another loadtier
   // call restarts at begin and the by-owner index absorbs only the newcomer.
   auto it = cursor == 0 ? owners.begin() : owners.upper_bound(roa::nodeowner_key{cursor});
   while (it != owners.end() && scanned < max_rows) {
      const auto owner = *it;
      ++it;
      ++scanned;
      cursor = owner.owner.value;
      if (owner.tier == raw_tier && by_owner.find(owner.owner.value) == by_owner.end()) {
         const uint64_t snapshot_index = static_cast<uint64_t>(already) + written;
         table.emplace(RAM_PAYER, council::index_key{snapshot_index}, Row{snapshot_index, owner.owner});
         ++written;
      }
   }
   const bool complete = it == owners.end();
   return snapshot_result{scanned, written, complete ? 0 : cursor, complete};
}

/// Return SHA-256 over the canonical packed representation of the supplied values.
template <typename... Args>
checksum256 hash_args(const Args&... args) {
   auto packed = pack(std::tie(args...));
   return sha256(packed.data(), packed.size());
}
} // namespace

// ===========================================================================
//  Entropy accumulator (Variant B — block number intentionally excluded)
// ===========================================================================
void council::do_stir(election_state& st, name action_tag, name actor) {
   ++st.stir_count;
   st.acc = hash_args(st.acc, action_tag, actor, st.stir_count);
}

// ===========================================================================
//  Cross-contract reads (sysio.roa)
// ===========================================================================
uint8_t council::roa_network_gen() const {
   return roa::current_network_gen(councl::ROA_ACCOUNT);
}

uint32_t council::tier_count(uint8_t network_gen, councl::election_tier tier) const {
   return roa::nodeowner_count(councl::ROA_ACCOUNT, network_gen, tier_integer(tier));
}

// ===========================================================================
//  Convenience lookups
// ===========================================================================
name council::roster_owner(const config_state& cfg, uint8_t seat) const {
   roster_t roster(get_self(), cfg.election_gen);
   return roster.get(index_key{seat}, "roster seat missing").owner;
}

uint32_t council::member_index(const config_state& cfg, councl::election_tier tier, name who) const {
   if (tier == councl::election_tier::T1) {
      roster_t r(get_self(), cfg.election_gen);
      return frozen_member_index(r, who);
   }
   if (tier == councl::election_tier::T2) {
      tier2_t t(get_self(), cfg.election_gen);
      return frozen_member_index(t, who);
   }
   check(tier == councl::election_tier::T3, "invalid election tier for membership lookup");
   tier3_t t(get_self(), cfg.election_gen);
   return frozen_member_index(t, who);
}

// ===========================================================================
//  Round state, constrained flights, and final tabulation
// ===========================================================================
void council::check_round(const config_state& cfg, const election_state& st, uint64_t generation,
                          uint64_t round) const {
   check(cfg.init_phase == councl::init_phase::READY, "election is not running");
   check(generation == cfg.election_gen, "election generation does not match");
   check(round == st.round_id, "round does not match");
}

void council::open_round(election_state& st) {
   check(st.round_id < std::numeric_limits<uint64_t>::max(), "round identity exhausted");
   ++st.round_id;
   st.phase = councl::election_phase::NOMINATING;
   st.round_open_ts = current_time_point();
   st.vote_deadline = time_point{};
   st.cursor = 0;
   st.t1_ballots = st.t2_ballots = st.t3_ballots = 0;
   st.flight_hash = checksum256{};
   st.round_seed = checksum256{};
}

void council::save_flight(const config_state& cfg, const election_state& st, uint8_t seat,
                          const std::vector<name>& candidates, bool automatic) {
   check(candidates.size() == councl::SLATE_SIZE, "flight must contain three candidates");
   check(candidates[0] != candidates[1] && candidates[0] != candidates[2] && candidates[1] != candidates[2],
         "slate candidates must be distinct");
   candidates_t registry(get_self(), cfg.election_gen);
   // Validate against the other seats before releasing anything. Retained own claims are valid.
   for (uint8_t pos = 0; pos < councl::SLATE_SIZE; ++pos) {
      const auto candidate = registry.get(cand_key{candidates[pos].value}, "candidate not registered");
      check(!candidate.elected, "candidate already elected to a seat");
      check(candidate.claim_round != st.round_id || candidate.positions[pos] == councl::NO_SEAT ||
               candidate.positions[pos] == seat,
            "candidate position is reserved by another flight");
   }

   release_flight(cfg, st, seat);
   for (uint8_t pos = 0; pos < councl::SLATE_SIZE; ++pos)
      registry.modify(same_payer, cand_key{candidates[pos].value}, [&](auto& row) {
         if (row.claim_round != st.round_id) {
            row.claim_round = st.round_id;
            row.positions.assign(councl::SLATE_SIZE, councl::NO_SEAT);
         }
         row.positions[pos] = seat;
      });
   flights_t(get_self(), cfg.election_gen)
      .set(RAM_PAYER, index_key{seat}, flight_row{seat, st.round_id, candidates, automatic});
}

void council::release_flight(const config_state& cfg, const election_state& st, uint8_t seat) {
   flights_t flights(get_self(), cfg.election_gen);
   const auto previous = flights.try_get(index_key{seat});
   if (!previous)
      return;
   candidates_t registry(get_self(), cfg.election_gen);
   if (previous->round_id == st.round_id)
      for (uint8_t pos = 0; pos < previous->candidates.size(); ++pos)
         registry.modify(same_payer, cand_key{previous->candidates[pos].value}, [&](auto& row) {
            if (row.claim_round == st.round_id && row.positions[pos] == seat)
               row.positions[pos] = councl::NO_SEAT;
         });
   flights.erase(index_key{seat});
}

void council::generate_flights(election_state& st, const config_state& cfg, uint32_t max_steps) {
   candidates_t registry(get_self(), cfg.election_gen);
   // One bounded pool read per transaction; subsequent constrained draws use memory only.
   std::vector<candidate_row> pool;
   pool.reserve(cfg.cand_count);
   for (auto it = registry.begin(); it != registry.end(); ++it) {
      check(pool.size() < councl::MAX_CANDIDATES, "candidate pool exceeds the safety limit");
      pool.push_back(*it);
   }
   flights_t flights(get_self(), cfg.election_gen);
   council_t results(get_self(), cfg.election_gen);
   for (uint32_t step = 0; step < max_steps && st.cursor < councl::SEATS; ++step, ++st.cursor) {
      const uint8_t seat = st.cursor;
      if (results.contains(index_key{seat}))
         continue;
      const auto existing = flights.try_get(index_key{seat});
      if (existing && existing->round_id == st.round_id)
         continue;
      std::vector<name> candidates;
      for (uint8_t pos = 0; pos < councl::SLATE_SIZE; ++pos) {
         std::vector<size_t> available;
         for (size_t i = 0; i < pool.size(); ++i) {
            const auto& candidate = pool[i];
            if (!candidate.elected &&
                (candidate.claim_round != st.round_id || candidate.positions[pos] == councl::NO_SEAT) &&
                std::find(candidates.begin(), candidates.end(), candidate.account) == candidates.end())
               available.push_back(i);
         }
         if (available.empty())
            break;
         const auto seed = councl_math::seed_u64(
            hash_args(st.round_seed, cfg.election_gen, st.round_id, seat, pos).extract_as_byte_array());
         candidates.push_back(pool[available[councl_math::bounded_index(seed, available.size())]].account);
      }
      // Defensive fallback only: the 23-candidate minimum guarantees completion under normal invariants.
      if (candidates.size() != councl::SLATE_SIZE)
         continue;
      save_flight(cfg, st, seat, candidates, true);
      for (uint8_t pos = 0; pos < councl::SLATE_SIZE; ++pos)
         for (auto& candidate : pool)
            if (candidate.account == candidates[pos]) {
               if (candidate.claim_round != st.round_id) {
                  candidate.claim_round = st.round_id;
                  candidate.positions.assign(councl::SLATE_SIZE, councl::NO_SEAT);
               }
               candidate.positions[pos] = seat;
               break;
            }
   }
   if (st.cursor == councl::SEATS)
      open_voting(st, cfg);
}

void council::open_voting(election_state& st, const config_state& cfg) {
   flights_t flights(get_self(), cfg.election_gen);
   council_t results(get_self(), cfg.election_gen);
   st.flight_hash = hash_args(cfg.election_gen, st.round_id);
   for (uint8_t seat = 0; seat < councl::SEATS; ++seat) {
      const auto flight = flights.try_get(index_key{seat});
      const std::vector<name> candidates =
         flight && flight->round_id == st.round_id && !results.contains(index_key{seat}) ? flight->candidates
                                                                                         : std::vector<name>{};
      st.flight_hash = hash_args(st.flight_hash, seat, candidates);
   }
   st.phase = councl::election_phase::VOTING;
   st.vote_deadline = current_time_point() + sysio::seconds(cfg.time_slot_sec);
   st.cursor = 0;
}

void council::seat_member(election_state& st, const config_state& cfg, uint8_t seat, name member,
                          councl::election_tier filled_tier) {
   check(seat < councl::SEATS, "invalid council seat");
   council_t results(get_self(), cfg.election_gen);
   check(!results.contains(index_key{seat}), "seat already filled");
   candidates_t registry(get_self(), cfg.election_gen);
   registry.modify(
      same_payer, cand_key{member.value},
      [&](auto& candidate) {
         check(!candidate.elected, "candidate already elected to a seat");
         candidate.elected = true;
      },
      "member is not a candidate");
   const name owner = roster_owner(cfg, seat);
   results.emplace(RAM_PAYER, index_key{seat}, council_row{seat, owner, filled_tier, owner, member, st.round_id});
   ++st.seats_filled;
   if (st.seats_filled == councl::SEATS)
      st.phase = councl::election_phase::DONE;
}

void council::tabulate(election_state& st, const config_state& cfg, uint32_t max_steps) {
   flights_t flights(get_self(), cfg.election_gen);
   council_t results(get_self(), cfg.election_gen);
   candidates_t registry(get_self(), cfg.election_gen);
   const std::array<uint32_t, 3> submitted_ballots{st.t1_ballots, st.t2_ballots, st.t3_ballots};
   const std::array<councl::election_tier, 3> tiers{councl::election_tier::T1, councl::election_tier::T2,
                                                    councl::election_tier::T3};
   for (uint32_t step = 0; step < max_steps && st.cursor < councl::SEATS; ++step, ++st.cursor) {
      const uint8_t seat = st.cursor;
      if (results.contains(index_key{seat}))
         continue;
      const auto flight = flights.try_get(index_key{seat});
      if (!flight || flight->round_id != st.round_id || flight->candidates.size() != councl::SLATE_SIZE)
         continue;
      std::array<bool, councl::SLATE_SIZE> elected{};
      for (uint8_t pos = 0; pos < councl::SLATE_SIZE; ++pos)
         elected[pos] = registry.get(cand_key{flight->candidates[pos].value}).elected;
      for (size_t tier = 0; tier < tiers.size(); ++tier) {
         const auto& tally = flight->tallies[tier];
         const auto resolution = resolve_final({tally.yes1, tally.yes2, tally.yes3}, elected, submitted_ballots[tier]);
         if (resolution.result == round_result::WIN) {
            seat_member(st, cfg, seat, flight->candidates[resolution.winner_index], tiers[tier]);
            break;
         }
      }
   }
   if (st.cursor == councl::SEATS && st.phase != councl::election_phase::DONE)
      st.phase = councl::election_phase::CONTINUING;
}

// ===========================================================================
//  Registration
// ===========================================================================
void council::addcandidate(name account, std::string handle) {
   require_auth(account);
   config_t cg(get_self());
   config_state cfg = cg.get_or_create(RAM_PAYER, config_state{});
   check(cfg.init_phase == councl::init_phase::REG, "candidate registration is closed");
   check(cfg.cand_count < councl::MAX_CANDIDATES, "candidate registration limit reached");
   check(valid_handle(handle), "handle contains invalid characters or length");

   candidates_t cands(get_self(), cfg.election_gen);
   cands.emplace(account, cand_key{account.value}, candidate_row{account, handle, false}, "already a candidate");

   ++cfg.cand_count;
   cg.set(cfg, RAM_PAYER);
}

void council::rmcandidate(name account) {
   require_auth(get_self());
   config_t cg(get_self());
   config_state cfg = cg.get("candidate registry does not exist");
   check(cfg.init_phase == councl::init_phase::REG, "candidate registration is closed");

   candidates_t cands(get_self(), cfg.election_gen);
   cands.erase(cand_key{account.value}, "not a candidate");

   --cfg.cand_count;
   cg.set(cfg, RAM_PAYER);
}

// ===========================================================================
//  Staged initialization
// ===========================================================================
void council::startinit(uint64_t time_slot_sec, std::vector<name> ordered_owners) {
   require_auth(get_self());
   config_t cg(get_self());
   config_state cfg = cg.get("candidate registry does not exist");
   check(cfg.init_phase == councl::init_phase::REG, "election initialization is not available");
   check(time_slot_sec > 0, "time_slot_sec must be positive");
   check(time_slot_sec <= councl::MAX_TIME_SLOT_SEC, "time_slot_sec exceeds the safety limit");
   check(cfg.cand_count >= councl::MIN_CANDIDATES, "fewer candidates than required");
   check(ordered_owners.size() == councl::SEATS, "ordered_owners must list every council seat owner");

   const uint8_t ng = roa_network_gen();
   const uint8_t t1_raw = magic_enum::enum_integer(NodeOwnerTier::NODE_OWNER_TIER_T1);

   // Enumerate roa's tier-1 owners (the authoritative set the ordering must permute).
   roa::nodeowners_t no(councl::ROA_ACCOUNT, ng);
   auto t1_idx = no.get_index<"bytier"_n>();
   std::vector<name> t1;
   for (auto it = t1_idx.lower_bound(t1_raw); it != t1_idx.end(); ++it) {
      if (it->tier != t1_raw)
         break;
      t1.push_back(it->owner);
   }
   check(t1.size() == councl::SEATS, "roa tier-1 owner count does not match the council seat count");

   // Compare sorted in-memory copies once instead of point-reading the byowner index while it is
   // being populated. Preserve ordered_owners itself as the governance-selected seat order.
   auto sorted_owners = ordered_owners;
   std::sort(sorted_owners.begin(), sorted_owners.end());
   check(std::adjacent_find(sorted_owners.begin(), sorted_owners.end()) == sorted_owners.end(),
         "duplicate owner in ordered_owners");
   std::sort(t1.begin(), t1.end());
   check(sorted_owners == t1, "ordered_owners contains a non tier-1 owner");

   roster_t roster(get_self(), cfg.election_gen);
   for (uint64_t i = 0; i < ordered_owners.size(); ++i) {
      const name owner = ordered_owners[i];
      roster.emplace(RAM_PAYER, index_key{i}, roster_row{i, owner});
   }

   cfg.network_gen = ng;
   cfg.time_slot_sec = time_slot_sec;
   cfg.init_phase = councl::init_phase::LOADING;
   cfg.t2_loaded = 0;
   cfg.t3_loaded = 0;
   cfg.t2_cursor = cfg.t3_cursor = 0;
   cfg.t2_scan_complete = cfg.t3_scan_complete = false;
   cg.set(cfg, RAM_PAYER);
}

void council::loadtier(uint8_t tier, uint32_t max_rows) {
   require_auth(get_self());
   auto election_tier = magic_enum::enum_cast<councl::election_tier>(tier);
   check(election_tier.has_value() &&
            (*election_tier == councl::election_tier::T2 || *election_tier == councl::election_tier::T3),
         "tier must be T2 or T3");
   check(max_rows > 0, "max_rows must be positive");
   check(max_rows <= councl::MAX_BATCH_ROWS, "max_rows exceeds the safety limit");
   config_t cg(get_self());
   config_state cfg = cg.get("contract not initialized");
   check(cfg.init_phase == councl::init_phase::LOADING, "not in the loading phase");

   roa::nodeowners_t no(councl::ROA_ACCOUNT, cfg.network_gen);
   // Resume in primary owner order from the last inspected identity. max_rows bounds source reads,
   // even when most rows belong to another tier or have already been snapshotted.
   const uint32_t already = *election_tier == councl::election_tier::T2 ? cfg.t2_loaded : cfg.t3_loaded;
   const uint8_t raw_tier = tier_integer(*election_tier);

   if (*election_tier == councl::election_tier::T2) {
      if (cfg.t2_scan_complete) {
         cfg.t2_cursor = 0;
         cfg.t2_scan_complete = false;
      }
      tier2_t t2(get_self(), cfg.election_gen);
      const auto result = append_snapshot<tier2_row>(t2, no, raw_tier, already, cfg.t2_cursor, max_rows);
      cfg.t2_loaded += result.written;
      cfg.t2_cursor = result.cursor;
      cfg.t2_scan_complete = result.complete;
      // Defense in depth: normal ROA registration enforces the same system cap before this row
      // can exist, but reject corrupt or incompatible cross-contract state explicitly.
      check(cfg.t2_loaded <= sysiosystem::emissions::T2_MAX_NODE_OWNERS,
            "tier-2 snapshot exceeds the system owner cap");
   } else {
      if (cfg.t3_scan_complete) {
         cfg.t3_cursor = 0;
         cfg.t3_scan_complete = false;
      }
      tier3_t t3(get_self(), cfg.election_gen);
      const auto result = append_snapshot<tier3_row>(t3, no, raw_tier, already, cfg.t3_cursor, max_rows);
      cfg.t3_loaded += result.written;
      cfg.t3_cursor = result.cursor;
      cfg.t3_scan_complete = result.complete;
      // Defense in depth; see the tier-2 cap check above.
      check(cfg.t3_loaded <= sysiosystem::emissions::T3_MAX_NODE_OWNERS,
            "tier-3 snapshot exceeds the system owner cap");
   }

   cg.set(cfg, RAM_PAYER);
}

void council::finalizeinit() {
   require_auth(get_self());
   config_t cg(get_self());
   config_state cfg = cg.get("contract not initialized");
   check(cfg.init_phase == councl::init_phase::LOADING, "not in the loading phase");

   // The snapshot scope and tier values together guarantee identity as well as count completeness.
   check(cfg.network_gen == roa_network_gen(), "roa network generation changed during initialization");
   check(cfg.t2_scan_complete, "tier-2 source scan incomplete");
   check(cfg.t3_scan_complete, "tier-3 source scan incomplete");
   const uint32_t c2 = tier_count(cfg.network_gen, councl::election_tier::T2);
   const uint32_t c3 = tier_count(cfg.network_gen, councl::election_tier::T3);
   check(c2 <= sysiosystem::emissions::T2_MAX_NODE_OWNERS, "tier-2 count exceeds the system owner cap");
   check(c3 <= sysiosystem::emissions::T3_MAX_NODE_OWNERS, "tier-3 count exceeds the system owner cap");
   check(cfg.t2_loaded == c2, "tier-2 snapshot incomplete");
   check(cfg.t3_loaded == c3, "tier-3 snapshot incomplete");

   cfg.n2 = c2;
   cfg.n3 = c3;
   cfg.init_phase = councl::init_phase::READY;
   cg.set(cfg, RAM_PAYER);

   election_state st{};
   st.acc = hash_args(ACC_SEED_TAG, cfg.election_gen);
   open_round(st);
   state_t(get_self()).set(st, RAM_PAYER);
}

void council::reset() {
   require_auth(get_self());
   config_t cg(get_self());
   config_state cfg = cg.get("contract not initialized");
   if (cfg.init_phase == councl::init_phase::LOADING) {
      // Candidate registration predates the failed snapshot and remains valid. Purge only staged
      // roster/tier rows and reopen REG in the same generation.
      cfg.cleanup_mode = councl::cleanup_mode::INIT_ABORT;
      cfg.cleanup_stage = councl::cleanup_stage::ROSTER;
   } else {
      check(cfg.init_phase == councl::init_phase::READY, "reset requires a loading or active election generation");
      state_t sg(get_self());
      const election_state st = sg.get("election state missing");
      cfg.cleanup_mode = st.phase == councl::election_phase::DONE ? councl::cleanup_mode::COMPLETED
                                                                  : councl::cleanup_mode::ACTIVE_ABORT;
      cfg.cleanup_stage = councl::cleanup_stage::CANDIDATES;
   }

   cfg.init_phase = councl::init_phase::CLEANING;
   cg.set(cfg, RAM_PAYER);
}

void council::purge(uint32_t max_rows) {
   require_auth(get_self());
   check(max_rows > 0, "max_rows must be positive");
   check(max_rows <= councl::MAX_BATCH_ROWS, "max_rows exceeds the safety limit");

   config_t cg(get_self());
   config_state cfg = cg.get("contract not initialized");
   check(cfg.init_phase == councl::init_phase::CLEANING, "generation cleanup is not active");
   check(cfg.cleanup_mode == councl::cleanup_mode::INIT_ABORT ||
            cfg.cleanup_mode == councl::cleanup_mode::ACTIVE_ABORT ||
            cfg.cleanup_mode == councl::cleanup_mode::COMPLETED,
         "unknown cleanup mode");

   uint32_t remaining = max_rows;
   while (remaining > 0 && cfg.cleanup_stage != councl::cleanup_stage::COMPLETE) {
      uint32_t erased = 0;
      switch (cfg.cleanup_stage) {
      case councl::cleanup_stage::CANDIDATES: {
         candidates_t table(get_self(), cfg.election_gen);
         const auto result = erase_rows(table, remaining);
         erased = result.erased;
         if (result.drained)
            cfg.cleanup_stage = councl::cleanup_stage::ROSTER;
         break;
      }
      case councl::cleanup_stage::ROSTER: {
         roster_t table(get_self(), cfg.election_gen);
         const auto result = erase_rows(table, remaining);
         erased = result.erased;
         if (result.drained)
            cfg.cleanup_stage = councl::cleanup_stage::TIER2;
         break;
      }
      case councl::cleanup_stage::TIER2: {
         tier2_t table(get_self(), cfg.election_gen);
         const auto result = erase_rows(table, remaining);
         erased = result.erased;
         if (result.drained)
            cfg.cleanup_stage = councl::cleanup_stage::TIER3;
         break;
      }
      case councl::cleanup_stage::TIER3: {
         tier3_t table(get_self(), cfg.election_gen);
         const auto result = erase_rows(table, remaining);
         erased = result.erased;
         if (result.drained) {
            cfg.cleanup_stage = cfg.cleanup_mode == councl::cleanup_mode::INIT_ABORT ? councl::cleanup_stage::COMPLETE
                                                                                     : councl::cleanup_stage::FLIGHTS;
         }
         break;
      }
      case councl::cleanup_stage::FLIGHTS: {
         flights_t table(get_self(), cfg.election_gen);
         const auto result = erase_rows(table, remaining);
         erased = result.erased;
         if (result.drained)
            cfg.cleanup_stage = councl::cleanup_stage::BALLOTS;
         break;
      }
      case councl::cleanup_stage::BALLOTS: {
         ballots_t table(get_self(), cfg.election_gen);
         const auto result = erase_rows(table, remaining);
         erased = result.erased;
         if (result.drained)
            cfg.cleanup_stage = cfg.cleanup_mode == councl::cleanup_mode::ACTIVE_ABORT
                                   ? councl::cleanup_stage::COUNCIL
                                   : councl::cleanup_stage::COMPLETE;
         break;
      }
      case councl::cleanup_stage::COUNCIL: {
         council_t table(get_self(), cfg.election_gen);
         const auto result = erase_rows(table, remaining);
         erased = result.erased;
         if (result.drained)
            cfg.cleanup_stage = councl::cleanup_stage::COMPLETE;
         break;
      }
      case councl::cleanup_stage::COMPLETE:
         break;
      default:
         check(false, "unknown cleanup stage");
      }
      remaining -= erased;
   }

   if (cfg.cleanup_stage == councl::cleanup_stage::COMPLETE) {
      check(cfg.cleanup_mode != councl::cleanup_mode::NONE, "cleanup mode is not set");
      const bool preserve_registry = cfg.cleanup_mode == councl::cleanup_mode::INIT_ABORT;
      if (!preserve_registry) {
         check(cfg.election_gen < std::numeric_limits<uint64_t>::max(), "election generation exhausted");
         ++cfg.election_gen;
         cfg.cand_count = 0;
      }
      cfg.init_phase = councl::init_phase::REG;
      cfg.time_slot_sec = 0;
      cfg.n2 = cfg.n3 = cfg.t2_loaded = cfg.t3_loaded = 0;
      cfg.t2_cursor = cfg.t3_cursor = 0;
      cfg.t2_scan_complete = cfg.t3_scan_complete = false;
      cfg.cleanup_mode = councl::cleanup_mode::NONE;
      state_t(get_self()).remove();
   }
   cg.set(cfg, RAM_PAYER);
}

// ===========================================================================
//  Election
// ===========================================================================
void council::repcandidate(name proposer, name c1, name c2, name c3, uint64_t election_gen, uint64_t round_id) {
   require_auth(proposer);
   const auto cfg = config_t(get_self()).get("contract not initialized");
   state_t sg(get_self());
   auto st = sg.get("election state missing");
   check_round(cfg, st, election_gen, round_id);
   check(st.phase == councl::election_phase::NOMINATING, "not accepting nominations right now");
   check(current_time_point() <= st.round_open_ts + sysio::seconds(cfg.time_slot_sec),
         "nomination deadline has elapsed");
   const auto seat = member_index(cfg, councl::election_tier::T1, proposer);
   check(seat != INVALID_MEMBER_INDEX, "only a frozen tier-1 owner may nominate");
   council_t results(get_self(), cfg.election_gen);
   check(!results.contains(index_key{seat}), "seat already filled");
   save_flight(cfg, st, seat, {c1, c2, c3}, false);
   do_stir(st, ACTION_REPCANDIDATE, proposer);
   sg.set(st, RAM_PAYER);
}

void council::vote(name voter, uint64_t election_gen, uint64_t round_id, checksum256 flight_hash,
                   std::vector<flight_vote> votes) {
   require_auth(voter);
   const auto cfg = config_t(get_self()).get("contract not initialized");
   state_t sg(get_self());
   auto st = sg.get("election state missing");
   check_round(cfg, st, election_gen, round_id);
   check(st.phase == councl::election_phase::VOTING, "voting is not open");
   check(current_time_point() <= st.vote_deadline, "voting deadline has elapsed");
   check(flight_hash == st.flight_hash, "flight set does not match");
   councl::election_tier tier = councl::election_tier::T1;
   const uint32_t own_seat = member_index(cfg, tier, voter);
   if (own_seat == INVALID_MEMBER_INDEX) {
      tier = councl::election_tier::T2;
      if (member_index(cfg, tier, voter) == INVALID_MEMBER_INDEX) {
         tier = councl::election_tier::T3;
         check(member_index(cfg, tier, voter) != INVALID_MEMBER_INDEX, "not eligible to vote");
      }
   }
   ballots_t ballots(get_self(), cfg.election_gen);
   const auto previous = ballots.try_get(cand_key{voter.value});
   check(!previous || previous->round_id != st.round_id, "already voted in this round");
   check(votes.size() <= councl::SEATS, "too many flight votes");
   flights_t flights(get_self(), cfg.election_gen);
   council_t results(get_self(), cfg.election_gen);
   size_t index = 0;
   for (uint8_t seat = 0; seat < councl::SEATS; ++seat) {
      if (seat == own_seat || results.contains(index_key{seat}))
         continue;
      const auto flight = flights.try_get(index_key{seat});
      if (!flight || flight->round_id != st.round_id || flight->candidates.size() != councl::SLATE_SIZE)
         continue;
      check(index < votes.size() && votes[index].seat == seat, "ballot must cover every eligible flight in seat order");
      const auto& decision = votes[index++];
      flights.modify(same_payer, index_key{seat}, [&](auto& row) {
         auto& tally = row.tallies[tier_integer(tier) - 1];
         ++tally.votes_cast;
         tally.yes1 += decision.v1;
         tally.yes2 += decision.v2;
         tally.yes3 += decision.v3;
      });
   }
   check(index == votes.size(), "ballot includes an ineligible flight");
   ballots.set(RAM_PAYER, cand_key{voter.value}, ballot_row{voter, st.round_id, tier, flight_hash, votes});
   switch (tier) {
   case councl::election_tier::T1:
      ++st.t1_ballots;
      break;
   case councl::election_tier::T2:
      ++st.t2_ballots;
      break;
   case councl::election_tier::T3:
      ++st.t3_ballots;
      break;
   }
   do_stir(st, ACTION_VOTE, voter);
   sg.set(st, RAM_PAYER);
}

void council::settle(name caller, uint64_t election_gen, uint64_t round_id, uint32_t max_steps) {
   require_auth(caller);
   check(max_steps > 0 && max_steps <= councl::SEATS, "max_steps must be between 1 and 21");
   const auto cfg = config_t(get_self()).get("contract not initialized");
   state_t sg(get_self());
   auto st = sg.get("election state missing");
   check_round(cfg, st, election_gen, round_id);
   if (st.phase == councl::election_phase::DONE)
      return;
   do_stir(st, ACTION_SETTLE, caller);
   switch (st.phase) {
   case councl::election_phase::NOMINATING:
      if (current_time_point() > st.round_open_ts + sysio::seconds(cfg.time_slot_sec)) {
         st.phase = councl::election_phase::GENERATING;
         st.cursor = 0;
         st.round_seed = hash_args(st.acc, cfg.election_gen, st.round_id);
      }
      break;
   case councl::election_phase::GENERATING:
      generate_flights(st, cfg, max_steps);
      break;
   case councl::election_phase::VOTING:
      if (current_time_point() > st.vote_deadline) {
         st.phase = councl::election_phase::TABULATING;
         st.cursor = 0;
      }
      break;
   case councl::election_phase::TABULATING:
      tabulate(st, cfg, max_steps);
      break;
   case councl::election_phase::CONTINUING:
      open_round(st);
      break;
   case councl::election_phase::DONE:
      break;
   }
   sg.set(st, RAM_PAYER);
}

void council::stir(name caller) {
   require_auth(caller);
   const auto cfg = config_t(get_self()).get("contract not initialized");
   check(cfg.init_phase == councl::init_phase::READY, "election is not running");
   state_t sg(get_self());
   auto st = sg.get("election state missing");
   do_stir(st, ACTION_STIR, caller);
   sg.set(st, RAM_PAYER);
}

} // namespace sysio
