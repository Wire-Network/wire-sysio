#include <array>
#include <limits>
#include <magic_enum/magic_enum.hpp>
#include <sysio.councl/sysio.councl.hpp>
#include <sysio.roa.hpp>              // roa::roastate_t / roa::nodeowners_t — tier membership
#include <sysio.system/emissions.hpp> // sysiosystem::emissions::nodecountstate_t — tier counts
#include <sysio/opp/types/types.pb.hpp>
#include <tuple>
#include <vector>

namespace sysio {

using councl_math::resolve;
using councl_math::round_result;

namespace {
// System-owned rows bill to the sysio RAM pool (privileged-contract model, as sysio.chalg
// does): the council account stays at code+abi size while row growth draws from the pool.
constexpr name ram_payer = councl::SYSTEM_ACCOUNT;

// Domain-separation tag for the entropy accumulator seed at election start.
constexpr name ACC_SEED_TAG = "councilseed"_n;

constexpr name ACTION_REPCANDIDATE = "repcandi"_n; ///< shortened on-chain tag for repcandidate
constexpr name ACTION_VOTE = "vote"_n;
constexpr name ACTION_SETTLE = "settle"_n;
constexpr name ACTION_FORCE_ASSIGN = "forceasgn"_n; ///< shortened on-chain tag for forceassign
constexpr name ACTION_FORCE_BACKSTOP = "forceback"_n;
constexpr name ACTION_STIR = "stir"_n;

constexpr uint64_t GR_SCOPE_X_BITS = 40;
constexpr uint64_t GR_SCOPE_GEN_BITS = 64 - GR_SCOPE_X_BITS;
constexpr uint64_t GR_SCOPE_X_LIMIT = uint64_t{1} << GR_SCOPE_X_BITS;
constexpr uint64_t GR_SCOPE_GEN_LIMIT = uint64_t{1} << GR_SCOPE_GEN_BITS;
constexpr uint32_t INVALID_MEMBER_INDEX = std::numeric_limits<uint32_t>::max();
constexpr uint32_t BITS_PER_BYTE = 8;

using NodeOwnerTier = opp::types::NodeOwnerTier;

static_assert(councl::SEATS == sysiosystem::emissions::T1_MAX_NODE_OWNERS,
              "council seats must match the system tier-1 owner cap");

/// Convert a council tier to its stable integer representation for ROA table comparisons.
constexpr uint8_t tier_integer(councl::election_tier tier) {
   return magic_enum::enum_integer(tier);
}

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

/// Erase at most `max_rows` rows and return the number removed.
template <typename Table>
uint32_t erase_rows(Table& table, uint32_t max_rows) {
   uint32_t erased = 0;
   auto it = table.begin();
   while (it != table.end() && erased < max_rows) {
      it = table.erase(it);
      ++erased;
   }
   return erased;
}

/// Return the stable index of `owner` in a typed frozen snapshot, or the invalid sentinel.
template <typename Table>
uint32_t frozen_member_index(Table& table, name owner) {
   auto by_owner = table.template get_index<"byowner"_n>();
   auto it = by_owner.find(owner.value);
   return it == by_owner.end() ? INVALID_MEMBER_INDEX : static_cast<uint32_t>(it->idx);
}

/// Append at most `max_rows` not-yet-snapshotted owners of `raw_tier` in deterministic order.
template <typename Row, typename Table, typename TierIndex>
uint32_t append_snapshot(Table& table, TierIndex& tier_index, uint8_t raw_tier, uint32_t already, uint32_t max_rows) {
   auto by_owner = table.template get_index<"byowner"_n>();
   uint32_t written = 0;
   for (auto it = tier_index.lower_bound(raw_tier); it != tier_index.end() && written < max_rows; ++it) {
      if (it->tier != raw_tier)
         break;
      if (by_owner.find(it->owner.value) != by_owner.end())
         continue;
      const uint64_t snapshot_index = static_cast<uint64_t>(already) + written;
      table.emplace(ram_payer, council::index_key{snapshot_index}, Row{snapshot_index, it->owner});
      ++written;
   }
   return written;
}

/// Return SHA-256 over the canonical packed representation of a tuple.
template <typename Tuple>
checksum256 hash_tuple(const Tuple& t) {
   auto packed = pack(t);
   return sha256(packed.data(), packed.size());
}
} // namespace

uint64_t council::gr_scope(uint64_t gen, uint64_t x) {
   check(gen < GR_SCOPE_GEN_LIMIT, "election generation exceeds scoped-table encoding");
   check(x < GR_SCOPE_X_LIMIT, "round or seat exceeds scoped-table encoding");
   return (gen << GR_SCOPE_X_BITS) | x;
}

// ===========================================================================
//  Entropy accumulator (Variant B — block number intentionally excluded)
// ===========================================================================
void council::do_stir(election_state& st, name action_tag, name actor) {
   ++st.stir_count;
   st.acc = hash_tuple(std::make_tuple(st.acc, action_tag, actor, st.stir_count));
}

// ===========================================================================
//  Cross-contract reads (sysio.roa / sysio.system)
// ===========================================================================
uint8_t council::roa_network_gen() const {
   roa::roastate_t rs(councl::ROA_ACCOUNT);
   check(rs.exists(), "roa state not initialized");
   return rs.get().network_gen;
}

uint32_t council::tier_count(councl::election_tier tier) const {
   sysiosystem::emissions::nodecountstate_t nc(councl::SYSTEM_ACCOUNT);
   check(nc.exists(), "sysio.system nodecount not initialized");
   auto v = nc.get();
   switch (tier) {
   case councl::election_tier::T1:
      return v.t1_count;
   case councl::election_tier::T2:
      return v.t2_count;
   case councl::election_tier::T3:
      return v.t3_count;
   case councl::election_tier::GOVERNANCE:
      break;
   }
   check(false, "governance is not a node-owner tier");
   return 0;
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
//  Pseudo-random tier proposer selection (§5)
// ===========================================================================
uint64_t council::selection_index(const election_state& st, uint32_t available) const {
   check(available > 0, "cannot select from an empty tier");
   // Seed folds in the seat and round_id so successive selections (even in one block) differ.
   const uint64_t seed = councl_math::seed_u64(
      hash_tuple(std::make_tuple(st.acc, static_cast<uint64_t>(st.active_seat), st.round_id)).extract_as_byte_array());
   return councl_math::bounded_index(seed, available);
}

name council::select_tier2_proposer(const election_state& st, const config_state& cfg) const {
   tier2_t t2(get_self(), cfg.election_gen);
   return t2.get(index_key{selection_index(st, cfg.n2)}, "tier-2 index out of range").owner;
}

name council::select_tier3_proposer(election_state& st, const config_state& cfg) {
   // Lazy Fisher-Yates remap: select and remove one virtual slot in O(1) KV operations.
   const uint32_t avail = st.tier3_available;
   const uint64_t j = selection_index(st, avail);
   tier3_remap_t remap(get_self(), gr_scope(cfg.election_gen, st.active_seat));
   const uint64_t last = avail - 1;
   const auto selected_key = index_key{j};
   const auto last_key = index_key{last};
   const uint64_t actual = remap.contains(selected_key) ? remap.get(selected_key).actual_idx : j;
   const uint64_t replacement = remap.contains(last_key) ? remap.get(last_key).actual_idx : last;

   if (j != last) {
      if (remap.contains(selected_key)) {
         remap.modify(ram_payer, selected_key, [&](auto& row) { row.actual_idx = replacement; });
      } else {
         remap.emplace(ram_payer, selected_key, remap_row{j, replacement});
      }
   }
   if (remap.contains(last_key))
      remap.erase(last_key);

   --st.tier3_available;
   tier3_t t3(get_self(), cfg.election_gen);
   return t3.get(index_key{actual}, "tier-3 index missing").owner;
}

// ===========================================================================
//  State machine internals
// ===========================================================================
void council::open_tier_attempt(election_state& st, councl::election_tier tier, name proposer) {
   st.tier = tier;
   st.proposer = proposer;
   ++st.round_id;
   st.phase = councl::election_phase::AWAIT_REP;
   st.round_open_ts = current_time_point();
   st.vote_deadline = time_point{};
   st.elect_N = 0;
   st.eligible_voters = 0;
   st.votes_cast = 0;
   st.voted_bitmap.clear();
   st.c1 = st.c2 = st.c3 = name{};
   st.yes1 = st.yes2 = st.yes3 = 0;
   st.no1 = st.no2 = st.no3 = 0;
}

void council::advance_seat(election_state& st, const config_state& cfg) {
   ++st.active_seat;
   st.tier3_available = cfg.n3;
   if (st.active_seat >= councl::SEATS) {
      st.phase = councl::election_phase::DONE;
      return;
   }
   open_tier_attempt(st, councl::election_tier::T1, roster_owner(cfg, st.active_seat));
}

void council::fail_attempt(election_state& st, const config_state& cfg) {
   if (st.tier == councl::election_tier::T1 && cfg.n2 > 0) {
      name proposer = select_tier2_proposer(st, cfg);
      open_tier_attempt(st, councl::election_tier::T2, proposer);
      return;
   }

   if (st.tier3_available == 0) {
      st.phase = councl::election_phase::BACKSTOP;
      return;
   }

   name proposer = select_tier3_proposer(st, cfg);
   open_tier_attempt(st, councl::election_tier::T3, proposer);
}

void council::seat_member(election_state& st, const config_state& cfg, name member, councl::election_tier filled_tier,
                          name proposer) {
   candidates_t cands(get_self(), cfg.election_gen);
   cands.modify(
      same_payer, cand_key{member.value},
      [&](auto& candidate) {
         check(!candidate.elected, "candidate already elected to a seat");
         candidate.elected = true;
      },
      "member is not a candidate");

   council_t council(get_self(), cfg.election_gen);
   council.emplace(ram_payer, index_key{st.active_seat},
                   council_row{
                      .seat = st.active_seat,
                      .seat_owner = roster_owner(cfg, st.active_seat),
                      .filled_tier = filled_tier,
                      .proposer = proposer,
                      .member = member,
                   });
   ++st.seats_filled;
   advance_seat(st, cfg);
}

void council::win_attempt(election_state& st, const config_state& cfg, name winner) {
   seat_member(st, cfg, winner, st.tier, st.proposer);
}

void council::try_resolve(election_state& st, const config_state& cfg) {
   if (st.phase != councl::election_phase::VOTING)
      return;

   const bool all_voted = st.votes_cast >= st.eligible_voters;
   // Both nomination and voting windows are inclusive at the exact deadline.
   const bool deadline = current_time_point() > st.vote_deadline;

   auto r = resolve(std::array<uint64_t, 3>{st.yes1, st.yes2, st.yes3}, std::array<uint64_t, 3>{st.no1, st.no2, st.no3},
                    st.elect_N, all_voted, deadline);

   if (r.result == round_result::PENDING)
      return;
   if (r.result == round_result::WIN) {
      name w = (r.winner_index == 0) ? st.c1 : (r.winner_index == 1 ? st.c2 : st.c3);
      win_attempt(st, cfg, w);
   } else {
      fail_attempt(st, cfg);
   }
}

void council::resolve_or_settle(election_state& st, const config_state& cfg) {
   if (st.phase == councl::election_phase::AWAIT_REP) {
      if (current_time_point() > st.round_open_ts + sysio::seconds(cfg.time_slot_sec))
         fail_attempt(st, cfg); // active proposer missed the nomination window
   } else if (st.phase == councl::election_phase::VOTING) {
      try_resolve(st, cfg);
   }
}

// ===========================================================================
//  Registration
// ===========================================================================
void council::addcandidate(name account, std::string handle) {
   require_auth(account);
   config_t cg(get_self());
   config_state cfg = cg.get_or_create(ram_payer, config_state{});
   check(cfg.init_phase == councl::init_phase::REG, "candidate registration is closed");
   check(cfg.cand_count < councl::MAX_CANDIDATES, "candidate registration limit reached");
   check(valid_handle(handle), "handle contains invalid characters or length");

   candidates_t cands(get_self(), cfg.election_gen);
   check(!cands.contains(cand_key{account.value}), "already a candidate");
   cands.emplace(account, cand_key{account.value}, candidate_row{account, handle, false});

   ++cfg.cand_count;
   cg.set(cfg, ram_payer);
}

void council::rmcandidate(name account) {
   require_auth(get_self());
   config_t cg(get_self());
   config_state cfg = cg.get("candidate registry does not exist");
   check(cfg.init_phase == councl::init_phase::REG, "candidate registration is closed");

   candidates_t cands(get_self(), cfg.election_gen);
   check(cands.contains(cand_key{account.value}), "not a candidate");
   cands.erase(cand_key{account.value});

   --cfg.cand_count;
   cg.set(cfg, ram_payer);
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

   // Freeze the ordered roster, verifying each entry is a distinct member of the roa tier-1 set.
   roster_t roster(get_self(), cfg.election_gen);
   auto by_owner = roster.get_index<"byowner"_n>();
   for (uint64_t i = 0; i < ordered_owners.size(); ++i) {
      name o = ordered_owners[i];
      bool in_t1 = false;
      for (const auto& x : t1) {
         if (x == o) {
            in_t1 = true;
            break;
         }
      }
      check(in_t1, "ordered_owners contains a non tier-1 owner");
      check(by_owner.find(o.value) == by_owner.end(), "duplicate owner in ordered_owners");
      roster.emplace(ram_payer, index_key{i}, roster_row{i, o});
   }

   cfg.network_gen = ng;
   cfg.time_slot_sec = time_slot_sec;
   cfg.init_phase = councl::init_phase::LOADING;
   cfg.t2_loaded = 0;
   cfg.t3_loaded = 0;
   cg.set(cfg, ram_payer);
}

void council::loadtier(uint8_t tier, uint32_t max_rows) {
   require_auth(get_self());
   auto election_tier = magic_enum::enum_cast<councl::election_tier>(tier);
   check(election_tier.has_value() &&
            (*election_tier == councl::election_tier::T2 || *election_tier == councl::election_tier::T3),
         "tier must be T2 or T3");
   check(max_rows > 0, "max_rows must be positive");
   config_t cg(get_self());
   config_state cfg = cg.get("contract not initialized");
   check(cfg.init_phase == councl::init_phase::LOADING, "not in the loading phase");

   roa::nodeowners_t no(councl::ROA_ACCOUNT, cfg.network_gen);
   auto idx = no.get_index<"bytier"_n>();
   // Resume by identity, not position: an owner is skipped iff it is already in the snapshot
   // (byowner lookup). A positional (skip-count) cursor mis-resumes when roa's tier set grows
   // between batches — a newcomer sorting before the cursor shifts the enumeration, duplicating
   // one owner and dropping another while finalizeinit's count cross-check still passes.
   // Identity dedup also absorbs such newcomers: whichever batch encounters them appends them,
   // so t{2,3}_loaded converges on the live nodecount and finalizeinit stays reachable.
   const uint32_t already = *election_tier == councl::election_tier::T2 ? cfg.t2_loaded : cfg.t3_loaded;
   const uint8_t raw_tier = tier_integer(*election_tier);

   if (*election_tier == councl::election_tier::T2) {
      tier2_t t2(get_self(), cfg.election_gen);
      const uint32_t written = append_snapshot<tier2_row>(t2, idx, raw_tier, already, max_rows);
      cfg.t2_loaded += written;
      check(cfg.t2_loaded <= sysiosystem::emissions::T2_MAX_NODE_OWNERS,
            "tier-2 snapshot exceeds the system owner cap");
   } else {
      tier3_t t3(get_self(), cfg.election_gen);
      const uint32_t written = append_snapshot<tier3_row>(t3, idx, raw_tier, already, max_rows);
      cfg.t3_loaded += written;
      check(cfg.t3_loaded <= sysiosystem::emissions::T3_MAX_NODE_OWNERS,
            "tier-3 snapshot exceeds the system owner cap");
   }

   cg.set(cfg, ram_payer);
}

void council::finalizeinit() {
   require_auth(get_self());
   config_t cg(get_self());
   config_state cfg = cg.get("contract not initialized");
   check(cfg.init_phase == councl::init_phase::LOADING, "not in the loading phase");

   // Completeness cross-check against sysio.system's live tier counts.
   const uint32_t c2 = tier_count(councl::election_tier::T2);
   const uint32_t c3 = tier_count(councl::election_tier::T3);
   check(c2 <= sysiosystem::emissions::T2_MAX_NODE_OWNERS, "tier-2 count exceeds the system owner cap");
   check(c3 <= sysiosystem::emissions::T3_MAX_NODE_OWNERS, "tier-3 count exceeds the system owner cap");
   check(cfg.t2_loaded == c2, "tier-2 snapshot incomplete");
   check(cfg.t3_loaded == c3, "tier-3 snapshot incomplete");

   cfg.n2 = c2;
   cfg.n3 = c3;
   cfg.init_phase = councl::init_phase::READY;
   cg.set(cfg, ram_payer);

   // Open seat 0 with a fresh accumulator seeded from the generation.
   election_state st{};
   st.acc = hash_tuple(std::make_tuple(ACC_SEED_TAG, cfg.election_gen));
   st.stir_count = 0;
   st.active_seat = 0;
   st.seats_filled = 0;
   st.tier3_available = cfg.n3;
   open_tier_attempt(st, councl::election_tier::T1, roster_owner(cfg, 0));
   state_t(get_self()).set(st, ram_payer);
}

void council::reset() {
   require_auth(get_self());
   config_t cg(get_self());
   config_state cfg = cg.get("contract not initialized");
   check(cfg.init_phase == councl::init_phase::READY, "reset requires a ready election generation");
   state_t sg(get_self());
   check(!sg.exists() || sg.get().phase == councl::election_phase::DONE, "current election is not complete");

   cfg.init_phase = councl::init_phase::CLEANING;
   cfg.cleanup_stage = councl::cleanup_stage::CANDIDATES;
   cfg.cleanup_seat = 0;
   cg.set(cfg, ram_payer);
}

void council::purge(uint32_t max_rows) {
   require_auth(get_self());
   check(max_rows > 0, "max_rows must be positive");

   config_t cg(get_self());
   config_state cfg = cg.get("contract not initialized");
   check(cfg.init_phase == councl::init_phase::CLEANING, "generation cleanup is not active");

   uint32_t remaining = max_rows;
   while (remaining > 0 && cfg.cleanup_stage != councl::cleanup_stage::COMPLETE) {
      uint32_t erased = 0;
      switch (cfg.cleanup_stage) {
      case councl::cleanup_stage::CANDIDATES: {
         candidates_t table(get_self(), cfg.election_gen);
         erased = erase_rows(table, remaining);
         if (table.begin() == table.end())
            cfg.cleanup_stage = councl::cleanup_stage::ROSTER;
         break;
      }
      case councl::cleanup_stage::ROSTER: {
         roster_t table(get_self(), cfg.election_gen);
         erased = erase_rows(table, remaining);
         if (table.begin() == table.end())
            cfg.cleanup_stage = councl::cleanup_stage::TIER2;
         break;
      }
      case councl::cleanup_stage::TIER2: {
         tier2_t table(get_self(), cfg.election_gen);
         erased = erase_rows(table, remaining);
         if (table.begin() == table.end())
            cfg.cleanup_stage = councl::cleanup_stage::TIER3;
         break;
      }
      case councl::cleanup_stage::TIER3: {
         tier3_t table(get_self(), cfg.election_gen);
         erased = erase_rows(table, remaining);
         if (table.begin() == table.end())
            cfg.cleanup_stage = councl::cleanup_stage::REMAP;
         break;
      }
      case councl::cleanup_stage::REMAP: {
         tier3_remap_t table(get_self(), gr_scope(cfg.election_gen, cfg.cleanup_seat));
         erased = erase_rows(table, remaining);
         if (table.begin() == table.end()) {
            ++cfg.cleanup_seat;
            if (cfg.cleanup_seat >= councl::SEATS)
               cfg.cleanup_stage = councl::cleanup_stage::COMPLETE;
         }
         break;
      }
      case councl::cleanup_stage::COMPLETE:
         break;
      }
      remaining -= erased;
   }

   if (cfg.cleanup_stage == councl::cleanup_stage::COMPLETE) {
      ++cfg.election_gen;
      cfg.init_phase = councl::init_phase::REG;
      cfg.time_slot_sec = 0;
      cfg.cand_count = 0;
      cfg.n2 = cfg.n3 = cfg.t2_loaded = cfg.t3_loaded = 0;
      cfg.cleanup_seat = 0;
   }
   cg.set(cfg, ram_payer);
}

// ===========================================================================
//  Election
// ===========================================================================
void council::repcandidate(name proposer, name c1, name c2, name c3) {
   require_auth(proposer);
   config_t cg(get_self());
   config_state cfg = cg.get("contract not initialized");
   check(cfg.init_phase == councl::init_phase::READY, "election is not running");

   state_t sg(get_self());
   election_state st = sg.get("election state missing");
   do_stir(st, ACTION_REPCANDIDATE, proposer);
   const uint64_t prior_round = st.round_id;
   const auto prior_phase = st.phase;
   resolve_or_settle(st, cfg);
   if (st.round_id != prior_round || st.phase != prior_phase) {
      sg.set(st, ram_payer);
      return; // stale nomination acted only as a caller-authenticated settlement crank
   }

   check(st.phase == councl::election_phase::AWAIT_REP, "not accepting nominations right now");
   check(proposer == st.proposer, "not your turn to nominate");
   check(c1 != c2 && c1 != c3 && c2 != c3, "slate candidates must be distinct");

   candidates_t cands(get_self(), cfg.election_gen);
   for (name c : {c1, c2, c3}) {
      auto cr = cands.get(cand_key{c.value}, "candidate not registered");
      check(!cr.elected, "candidate already elected to a seat");
   }

   // Open the voting round; tier-2/3 seed the proposer's auto-yes on all three candidates.
   st.c1 = c1;
   st.c2 = c2;
   st.c3 = c3;
   uint32_t bitmap_members = 0;
   if (st.tier == councl::election_tier::T1) {
      st.elect_N = councl::T1_VOTERS;
      st.eligible_voters = councl::T1_VOTERS;
      bitmap_members = councl::SEATS;
      st.yes1 = st.yes2 = st.yes3 = 0;
   } else {
      const uint32_t N = st.tier == councl::election_tier::T2 ? cfg.n2 : cfg.n3;
      st.elect_N = N;
      st.eligible_voters = (N > 0) ? (N - 1) : 0;
      bitmap_members = N;
      st.yes1 = st.yes2 = st.yes3 = 1; // proposer auto-yes
   }
   st.voted_bitmap.assign((bitmap_members + BITS_PER_BYTE - 1) / BITS_PER_BYTE, 0);
   st.no1 = st.no2 = st.no3 = 0;
   st.votes_cast = 0;
   st.phase = councl::election_phase::VOTING;
   st.vote_deadline = current_time_point() + sysio::seconds(cfg.time_slot_sec);

   try_resolve(st, cfg); // resolves immediately when the electorate is trivially small
   sg.set(st, ram_payer);
}

void council::vote(name voter, bool v1, bool v2, bool v3) {
   require_auth(voter);
   config_t cg(get_self());
   config_state cfg = cg.get("contract not initialized");
   check(cfg.init_phase == councl::init_phase::READY, "election is not running");

   state_t sg(get_self());
   election_state st = sg.get("election state missing");
   do_stir(st, ACTION_VOTE, voter);
   const uint64_t prior_round = st.round_id;
   const auto prior_phase = st.phase;
   resolve_or_settle(st, cfg);
   if (st.round_id != prior_round || st.phase != prior_phase) {
      sg.set(st, ram_payer);
      return; // stale vote acted only as a caller-authenticated settlement crank
   }

   check(st.phase == councl::election_phase::VOTING, "voting is not open");
   check(voter != st.proposer, "the proposer cannot vote on their own slate");
   const uint32_t voter_index = member_index(cfg, st.tier, voter);
   check(voter_index != INVALID_MEMBER_INDEX, "not eligible to vote in this tier");
   const uint32_t byte_index = voter_index / BITS_PER_BYTE;
   const uint8_t bit_mask = uint8_t{1} << (voter_index % BITS_PER_BYTE);
   check(byte_index < st.voted_bitmap.size(), "voter index exceeds the frozen tier snapshot");
   check((st.voted_bitmap[byte_index] & bit_mask) == 0, "already voted in this round");
   st.voted_bitmap[byte_index] |= bit_mask;

   if (v1)
      ++st.yes1;
   else
      ++st.no1;
   if (v2)
      ++st.yes2;
   else
      ++st.no2;
   if (v3)
      ++st.yes3;
   else
      ++st.no3;
   ++st.votes_cast;

   try_resolve(st, cfg);
   sg.set(st, ram_payer);
}

void council::settle(name caller) {
   require_auth(caller);
   config_t cg(get_self());
   config_state cfg = cg.get("contract not initialized");
   check(cfg.init_phase == councl::init_phase::READY, "election is not running");

   state_t sg(get_self());
   election_state st = sg.get("election state missing");
   check(st.phase != councl::election_phase::DONE, "election is complete");
   do_stir(st, ACTION_SETTLE, caller);
   resolve_or_settle(st, cfg);
   sg.set(st, ram_payer);
}

void council::forceback() {
   require_auth(get_self());
   config_t cg(get_self());
   config_state cfg = cg.get("contract not initialized");
   check(cfg.init_phase == councl::init_phase::READY, "election is not running");

   state_t sg(get_self());
   election_state st = sg.get("election state missing");
   check(st.phase == councl::election_phase::AWAIT_REP || st.phase == councl::election_phase::VOTING,
         "no active attempt is eligible for governance recovery");

   const bool elapsed = st.phase == councl::election_phase::AWAIT_REP
                           ? current_time_point() > st.round_open_ts + sysio::seconds(cfg.time_slot_sec)
                           : current_time_point() > st.vote_deadline;
   check(elapsed, "the active attempt has not elapsed");

   do_stir(st, ACTION_FORCE_BACKSTOP, get_self());
   st.phase = councl::election_phase::BACKSTOP;
   sg.set(st, ram_payer);
}

void council::forceassign(name member) {
   require_auth(get_self());
   config_t cg(get_self());
   config_state cfg = cg.get("contract not initialized");
   check(cfg.init_phase == councl::init_phase::READY, "election is not running");

   state_t sg(get_self());
   election_state st = sg.get("election state missing");
   do_stir(st, ACTION_FORCE_ASSIGN, get_self());
   check(st.phase == councl::election_phase::BACKSTOP, "not awaiting a governance assignment");

   seat_member(st, cfg, member, councl::election_tier::GOVERNANCE, get_self());
   sg.set(st, ram_payer);
}

void council::stir(name caller) {
   require_auth(caller);
   config_t cg(get_self());
   config_state cfg = cg.get("contract not initialized");
   check(cfg.init_phase == councl::init_phase::READY, "election is not running");
   state_t sg(get_self());
   check(sg.exists(), "election has not started");
   election_state st = sg.get();
   check(st.phase != councl::election_phase::DONE, "election is complete");
   do_stir(st, ACTION_STIR, caller);
   resolve_or_settle(st, cfg);
   sg.set(st, ram_payer);
}

} // namespace sysio
