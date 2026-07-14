#include <sysio.councl/sysio.councl.hpp>

#include <sysio.roa.hpp>              // roa::roastate_t / roa::nodeowners_t — tier membership
#include <sysio.system/emissions.hpp> // sysiosystem::emissions::nodecountstate_t — tier counts
#include <magic_enum/magic_enum.hpp>

#include <array>
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

   /// sha256 over the packed tuple, returned as a raw 32-byte array (for the pure seed helpers).
   template<typename Tuple>
   std::array<uint8_t, 32> hash_tuple(const Tuple& t) {
      auto packed = pack(t);
      checksum256 h = sha256(packed.data(), packed.size());
      return h.extract_as_byte_array();
   }
}

// ===========================================================================
//  Entropy accumulator (Variant B — block number intentionally excluded)
// ===========================================================================
void council::do_stir(election_state& st, name action_tag, name actor) {
   ++st.stir_count;
   auto packed = pack(std::make_tuple(st.acc, action_tag, actor, st.stir_count));
   st.acc = sha256(packed.data(), packed.size());
}

// ===========================================================================
//  Cross-contract reads (sysio.roa / sysio.system)
// ===========================================================================
uint8_t council::roa_network_gen() const {
   roa::roastate_t rs(councl::ROA_ACCOUNT);
   check(rs.exists(), "roa state not initialized");
   return rs.get().network_gen;
}

uint32_t council::tier_count(uint8_t tier) const {
   sysiosystem::emissions::nodecountstate_t nc(councl::SYSTEM_ACCOUNT);
   check(nc.exists(), "sysio.system nodecount not initialized");
   auto v = nc.get();
   return tier == 1 ? v.t1_count : (tier == 2 ? v.t2_count : v.t3_count);
}

// ===========================================================================
//  Convenience lookups
// ===========================================================================
name council::roster_owner(const config_state& cfg, uint8_t seat) const {
   roster_t roster(get_self(), cfg.election_gen);
   return roster.get(index_key{seat}, "roster seat missing").owner;
}

bool council::is_member(const config_state& cfg, uint8_t tier, name who) const {
   if (tier == 1) {
      roster_t r(get_self(), cfg.election_gen);
      auto idx = r.get_index<"byowner"_n>();
      return idx.find(who.value) != idx.end();
   }
   if (tier == 2) {
      tier2_t t(get_self(), cfg.election_gen);
      auto idx = t.get_index<"byowner"_n>();
      return idx.find(who.value) != idx.end();
   }
   tier3_t t(get_self(), cfg.election_gen);
   auto idx = t.get_index<"byowner"_n>();
   return idx.find(who.value) != idx.end();
}

// ===========================================================================
//  Pseudo-random tier proposer selection (§5)
// ===========================================================================
name council::select_from_tier(election_state& st, uint8_t tier, uint32_t avail,
                               const config_state& cfg) {
   // Seed folds in the seat and round_id so successive selections (even in one block) differ.
   const uint64_t seed = councl_math::seed_u64(
      hash_tuple(std::make_tuple(st.acc, static_cast<uint64_t>(st.active_seat), st.round_id)));
   const uint64_t j = councl_math::bounded_index(seed, avail);

   if (tier == 2) {
      tier2_t t2(get_self(), cfg.election_gen);
      return t2.get(index_key{j}, "tier-2 index out of range").owner;
   }

   // tier 3: walk indices skipping already-tried entries to the j-th survivor.
   tier3_t  t3(get_self(), cfg.election_gen);
   tried3_t tried(get_self(), gr_scope(cfg.election_gen, st.active_seat));
   uint64_t survivor = 0;
   for (uint64_t i = 0; i < cfg.n3; ++i) {
      if (tried.contains(index_key{i})) continue;
      if (survivor == j) {
         tried.emplace(ram_payer, index_key{i}, tried_row{i});
         ++st.tried_count;
         return t3.get(index_key{i}, "tier-3 index missing").owner;
      }
      ++survivor;
   }
   check(false, "tier-3 selection out of range");
   return name{};
}

// ===========================================================================
//  State machine internals
// ===========================================================================
void council::open_tier_attempt(election_state& st, const config_state& cfg, uint8_t tier,
                                name proposer) {
   st.tier            = tier;
   st.proposer        = proposer;
   ++st.round_id;
   st.phase           = councl::PH_AWAIT_REP;
   st.round_open_ts   = current_time_point();
   st.vote_deadline   = time_point{};
   st.elect_N         = 0;
   st.eligible_voters = 0;
   st.votes_cast      = 0;
   st.c1 = st.c2 = st.c3 = name{};
   st.yes1 = st.yes2 = st.yes3 = 0;
   st.no1  = st.no2  = st.no3  = 0;
}

void council::advance_seat(election_state& st, const config_state& cfg) {
   ++st.active_seat;
   st.tried_count = 0; // tier-3 tried set is scoped per seat; just reset the live counter
   if (st.active_seat >= councl::SEATS) {
      st.phase = councl::PH_DONE;
      return;
   }
   open_tier_attempt(st, cfg, 1, roster_owner(cfg, st.active_seat));
}

void council::fail_attempt(election_state& st, const config_state& cfg) {
   uint8_t next = (st.tier == 1) ? 2 : 3; // T1->T2, T2->T3, T3->T3 (retry)
   for (;;) {
      if (next == 2) {
         if (cfg.n2 == 0) { next = 3; continue; }
         name p = select_from_tier(st, 2, cfg.n2, cfg);
         open_tier_attempt(st, cfg, 2, p);
         return;
      }
      // tier 3
      const uint32_t avail = (cfg.n3 > st.tried_count) ? (cfg.n3 - st.tried_count) : 0;
      if (cfg.n3 == 0 || avail == 0) {
         st.phase = councl::PH_BACKSTOP; // tier-3 exhausted -> governance fills the seat
         return;
      }
      name p = select_from_tier(st, 3, avail, cfg);
      open_tier_attempt(st, cfg, 3, p);
      return;
   }
}

void council::win_attempt(election_state& st, const config_state& cfg, name winner) {
   candidates_t cands(get_self(), cfg.election_gen);
   cands.modify(ram_payer, cand_key{winner.value},
                [&](auto& c) { c.elected = true; }, "winner is not a candidate");

   council_t council(get_self(), cfg.election_gen);
   council.emplace(ram_payer, index_key{st.active_seat}, council_row{
      .seat        = st.active_seat,
      .seat_owner  = roster_owner(cfg, st.active_seat),
      .filled_tier = st.tier,
      .proposer    = st.proposer,
      .member      = winner,
   });
   ++st.seats_filled;
   advance_seat(st, cfg);
}

void council::try_resolve(election_state& st, const config_state& cfg) {
   if (st.phase != councl::PH_VOTING) return;

   const bool all_voted = st.votes_cast >= st.eligible_voters;
   const bool deadline  = current_time_point() >= st.vote_deadline;

   auto r = resolve(std::array<uint64_t, 3>{st.yes1, st.yes2, st.yes3},
                    std::array<uint64_t, 3>{st.no1, st.no2, st.no3},
                    st.elect_N, all_voted, deadline);

   if (r.result == round_result::PENDING) return;
   if (r.result == round_result::WIN) {
      name w = (r.winner_index == 0) ? st.c1 : (r.winner_index == 1 ? st.c2 : st.c3);
      win_attempt(st, cfg, w);
   } else {
      fail_attempt(st, cfg);
   }
}

void council::settle_if_elapsed(election_state& st, const config_state& cfg) {
   if (st.phase == councl::PH_AWAIT_REP) {
      if (current_time_point() > st.round_open_ts + sysio::seconds(cfg.time_slot_sec))
         fail_attempt(st, cfg); // active proposer missed the nomination window
   } else if (st.phase == councl::PH_VOTING) {
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
   check(cfg.init_phase == councl::IP_REG && cfg.reg_open, "candidate registration is closed");
   check(!handle.empty() && handle.size() <= councl::MAX_HANDLE_LEN, "handle length invalid");

   candidates_t cands(get_self(), cfg.election_gen);
   check(!cands.contains(cand_key{account.value}), "already a candidate");
   cands.emplace(ram_payer, cand_key{account.value}, candidate_row{account, handle, false});

   ++cfg.cand_count;
   cg.set(cfg, ram_payer);
}

void council::rmcandidate(name account) {
   require_auth(get_self());
   config_t cg(get_self());
   config_state cfg = cg.get("contract not initialized");
   check(cfg.init_phase == councl::IP_REG, "candidate registration is closed");

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
   config_state cfg = cg.get("no candidates registered");
   check(cfg.init_phase == councl::IP_REG, "init already started");
   check(time_slot_sec > 0, "time_slot_sec must be positive");
   check(cfg.cand_count >= councl::MIN_CANDIDATES, "need at least 23 registered candidates");
   check(ordered_owners.size() == councl::SEATS, "ordered_owners must list all 21 tier-1 owners");

   const uint8_t ng = roa_network_gen();
   const uint8_t T1 = magic_enum::enum_integer(NodeOwnerTier::NODE_OWNER_TIER_T1);

   // Enumerate roa's tier-1 owners (the authoritative set the ordering must permute).
   roa::nodeowners_t no(councl::ROA_ACCOUNT, ng);
   auto t1_idx = no.get_index<"bytier"_n>();
   std::vector<name> t1;
   for (auto it = t1_idx.lower_bound(static_cast<uint64_t>(T1)); it != t1_idx.end(); ++it) {
      if (it->tier != T1) break;
      t1.push_back(it->owner);
   }
   check(t1.size() == councl::SEATS, "roa does not have exactly 21 tier-1 node owners");

   // Freeze the ordered roster, verifying each entry is a distinct member of the roa tier-1 set.
   roster_t roster(get_self(), cfg.election_gen);
   auto by_owner = roster.get_index<"byowner"_n>();
   for (uint64_t i = 0; i < ordered_owners.size(); ++i) {
      name o = ordered_owners[i];
      bool in_t1 = false;
      for (const auto& x : t1) { if (x == o) { in_t1 = true; break; } }
      check(in_t1, "ordered_owners contains a non tier-1 owner");
      check(by_owner.find(o.value) == by_owner.end(), "duplicate owner in ordered_owners");
      roster.emplace(ram_payer, index_key{i}, roster_row{i, o});
   }

   cfg.network_gen   = ng;
   cfg.time_slot_sec = time_slot_sec;
   cfg.reg_open      = false;
   cfg.init_phase    = councl::IP_LOADING;
   cfg.t2_loaded     = 0;
   cfg.t3_loaded     = 0;
   cg.set(cfg, ram_payer);
}

void council::loadtier(uint8_t tier, uint32_t max_rows) {
   require_auth(get_self());
   check(tier == 2 || tier == 3, "tier must be 2 or 3");
   config_t cg(get_self());
   config_state cfg = cg.get("contract not initialized");
   check(cfg.init_phase == councl::IP_LOADING, "not in the loading phase");

   roa::nodeowners_t no(councl::ROA_ACCOUNT, cfg.network_gen);
   auto idx = no.get_index<"bytier"_n>();
   // Resume by identity, not position: an owner is skipped iff it is already in the snapshot
   // (byowner lookup). A positional (skip-count) cursor mis-resumes when roa's tier set grows
   // between batches — a newcomer sorting before the cursor shifts the enumeration, duplicating
   // one owner and dropping another while finalizeinit's count cross-check still passes.
   // Identity dedup also absorbs such newcomers: whichever batch encounters them appends them,
   // so t{2,3}_loaded converges on the live nodecount and finalizeinit stays reachable.
   const uint32_t already = (tier == 2) ? cfg.t2_loaded : cfg.t3_loaded;
   uint32_t written = 0;

   if (tier == 2) {
      tier2_t t2(get_self(), cfg.election_gen);
      auto by_owner = t2.get_index<"byowner"_n>();
      for (auto it = idx.lower_bound(static_cast<uint64_t>(2)); it != idx.end() && written < max_rows; ++it) {
         if (it->tier != 2) break;
         if (by_owner.find(it->owner.value) != by_owner.end()) continue; // already snapshotted
         t2.emplace(ram_payer, index_key{already + written}, tier2_row{already + written, it->owner});
         ++written;
      }
      cfg.t2_loaded += written;
   } else {
      tier3_t t3(get_self(), cfg.election_gen);
      auto by_owner = t3.get_index<"byowner"_n>();
      for (auto it = idx.lower_bound(static_cast<uint64_t>(3)); it != idx.end() && written < max_rows; ++it) {
         if (it->tier != 3) break;
         if (by_owner.find(it->owner.value) != by_owner.end()) continue; // already snapshotted
         t3.emplace(ram_payer, index_key{already + written}, tier3_row{already + written, it->owner});
         ++written;
      }
      cfg.t3_loaded += written;
   }

   cg.set(cfg, ram_payer);
}

void council::finalizeinit() {
   require_auth(get_self());
   config_t cg(get_self());
   config_state cfg = cg.get("contract not initialized");
   check(cfg.init_phase == councl::IP_LOADING, "not in the loading phase");

   // Completeness cross-check against sysio.system's live tier counts.
   const uint32_t c2 = tier_count(2);
   const uint32_t c3 = tier_count(3);
   check(cfg.t2_loaded == c2, "tier-2 snapshot incomplete");
   check(cfg.t3_loaded == c3, "tier-3 snapshot incomplete");

   cfg.n2          = c2;
   cfg.n3          = c3;
   cfg.init_phase  = councl::IP_READY;
   cfg.initialized = true;
   cg.set(cfg, ram_payer);

   // Open seat 0 with a fresh accumulator seeded from the generation.
   election_state st{};
   auto seed_bytes = pack(std::make_tuple(ACC_SEED_TAG, cfg.election_gen));
   st.acc          = sha256(seed_bytes.data(), seed_bytes.size());
   st.stir_count   = 0;
   st.active_seat  = 0;
   st.seats_filled = 0;
   st.tried_count  = 0;
   open_tier_attempt(st, cfg, 1, roster_owner(cfg, 0));
   state_t(get_self()).set(st, ram_payer);
}

void council::reset() {
   require_auth(get_self());
   config_t cg(get_self());
   config_state cfg = cg.get("contract not initialized");
   check(cfg.init_phase == councl::IP_READY, "an election is still in progress");
   state_t sg(get_self());
   check(!sg.exists() || sg.get().phase == councl::PH_DONE, "current election is not complete");

   // Bump the generation and reopen registration; prior-gen rows stay under their old scope.
   ++cfg.election_gen;
   cfg.init_phase  = councl::IP_REG;
   cfg.reg_open    = true;
   cfg.initialized = false;
   cfg.cand_count  = 0;
   cfg.n2 = cfg.n3 = cfg.t2_loaded = cfg.t3_loaded = 0;
   cg.set(cfg, ram_payer);
}

// ===========================================================================
//  Election
// ===========================================================================
void council::repcandidate(name proposer, name c1, name c2, name c3) {
   require_auth(proposer);
   config_t cg(get_self());
   config_state cfg = cg.get("contract not initialized");
   check(cfg.init_phase == councl::IP_READY, "election is not running");

   state_t sg(get_self());
   election_state st = sg.get("election state missing");
   do_stir(st, "repcandi"_n, proposer);
   settle_if_elapsed(st, cfg);

   check(st.phase == councl::PH_AWAIT_REP, "not accepting nominations right now");
   check(proposer == st.proposer, "not your turn to nominate");
   check(current_time_point() <= st.round_open_ts + sysio::seconds(cfg.time_slot_sec),
         "nomination window has passed");
   check(c1 != c2 && c1 != c3 && c2 != c3, "slate candidates must be distinct");

   candidates_t cands(get_self(), cfg.election_gen);
   for (name c : {c1, c2, c3}) {
      auto cr = cands.get(cand_key{c.value}, "candidate not registered");
      check(!cr.elected, "candidate already elected to a seat");
   }

   // Open the voting round; tier-2/3 seed the proposer's auto-yes on all three candidates.
   st.c1 = c1; st.c2 = c2; st.c3 = c3;
   if (st.tier == 1) {
      st.elect_N         = councl::T1_VOTERS;
      st.eligible_voters = councl::T1_VOTERS;
      st.yes1 = st.yes2 = st.yes3 = 0;
   } else {
      const uint32_t N   = (st.tier == 2) ? cfg.n2 : cfg.n3;
      st.elect_N         = N;
      st.eligible_voters = (N > 0) ? (N - 1) : 0;
      st.yes1 = st.yes2 = st.yes3 = 1; // proposer auto-yes
   }
   st.no1 = st.no2 = st.no3 = 0;
   st.votes_cast    = 0;
   st.phase         = councl::PH_VOTING;
   st.vote_deadline = current_time_point() + sysio::seconds(cfg.time_slot_sec);

   try_resolve(st, cfg); // resolves immediately when the electorate is trivially small
   sg.set(st, ram_payer);
}

void council::vote(name voter, bool v1, bool v2, bool v3) {
   require_auth(voter);
   config_t cg(get_self());
   config_state cfg = cg.get("contract not initialized");
   check(cfg.init_phase == councl::IP_READY, "election is not running");

   state_t sg(get_self());
   election_state st = sg.get("election state missing");
   do_stir(st, "vote"_n, voter);
   settle_if_elapsed(st, cfg);

   check(st.phase == councl::PH_VOTING, "voting is not open");
   check(voter != st.proposer, "the proposer cannot vote on their own slate");
   check(is_member(cfg, st.tier, voter), "not eligible to vote in this tier");

   ballots_t ballots(get_self(), gr_scope(cfg.election_gen, st.round_id));
   check(!ballots.contains(voter_key{voter.value}), "already voted in this round");
   ballots.emplace(ram_payer, voter_key{voter.value}, ballot_row{voter, v1, v2, v3});

   if (v1) ++st.yes1; else ++st.no1;
   if (v2) ++st.yes2; else ++st.no2;
   if (v3) ++st.yes3; else ++st.no3;
   ++st.votes_cast;

   try_resolve(st, cfg);
   sg.set(st, ram_payer);
}

void council::settle() {
   config_t cg(get_self());
   config_state cfg = cg.get("contract not initialized");
   check(cfg.init_phase == councl::IP_READY, "election is not running");

   state_t sg(get_self());
   election_state st = sg.get("election state missing");
   do_stir(st, "settle"_n, get_self());
   settle_if_elapsed(st, cfg);
   sg.set(st, ram_payer);
}

void council::forceassign(name member) {
   require_auth(get_self());
   config_t cg(get_self());
   config_state cfg = cg.get("contract not initialized");

   state_t sg(get_self());
   election_state st = sg.get("election state missing");
   do_stir(st, "forceasgn"_n, get_self());
   check(st.phase == councl::PH_BACKSTOP, "not awaiting a governance assignment");

   candidates_t cands(get_self(), cfg.election_gen);
   auto cr = cands.get(cand_key{member.value}, "member is not a candidate");
   check(!cr.elected, "candidate already elected to a seat");
   cands.modify(ram_payer, cand_key{member.value},
                [&](auto& c) { c.elected = true; }, "member is not a candidate");

   council_t council(get_self(), cfg.election_gen);
   council.emplace(ram_payer, index_key{st.active_seat}, council_row{
      .seat        = st.active_seat,
      .seat_owner  = roster_owner(cfg, st.active_seat),
      .filled_tier = councl::TIER_GOV,
      .proposer    = get_self(),
      .member      = member,
   });
   ++st.seats_filled;
   advance_seat(st, cfg);
   sg.set(st, ram_payer);
}

void council::stir() {
   config_t cg(get_self());
   config_state cfg = cg.get("contract not initialized");
   state_t sg(get_self());
   check(sg.exists(), "election has not started");
   election_state st = sg.get();
   do_stir(st, "stir"_n, get_self());
   sg.set(st, ram_payer);
}

} // namespace sysio
