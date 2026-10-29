#include <magic_enum/magic_enum.hpp>
#include <sysio.councl/sysio.councl.hpp>
#include <sysio.system/emissions.hpp>

#include <algorithm>
#include <limits>
#include <string_view>
#include <sysio.roa.hpp>

namespace sysio {
namespace {
constexpr name RAM_PAYER = councl::SYSTEM_ACCOUNT;
constexpr std::string_view HTTPS_PREFIX = "https://";
constexpr auto MISSING_WIP = "WIP not found";
constexpr auto CLOSED_WIP = "WIP is closed";
constexpr auto WRONG_REVISION = "WIP content hash does not match";
constexpr auto ENDORSEMENTS_MISSING = "WIP endorsement threshold not reached";
constexpr auto INVALID_WINDOW = "WIP windows must be positive and at most thirty days each";
constexpr auto INVALID_URI = "WIP URI must be bounded printable HTTPS";
constexpr auto EMPTY_HASH = "WIP content hash must not be zero";
constexpr auto INVALID_COUNCIL = "WIP requires a complete elected Council";
constexpr auto INVALID_T2 = "WIP requires a nonempty Tier-2 snapshot within the owner cap";
constexpr auto INVALID_POLICY = "WIP thresholds must be positive and reachable in the frozen electorates";
constexpr auto EXHAUSTED_IDS = "WIP identity exhausted";
constexpr auto ENDORSEMENT_CLOSED = "WIP endorsement window is closed";
constexpr auto NOT_ENDORSER = "account is not a frozen WIP Tier-2 owner";
constexpr auto DUPLICATE_ENDORSEMENT = "account already endorsed this WIP";
constexpr auto VOTING_CLOSED = "WIP voting window is not open";
constexpr auto NOT_MEMBER = "account is not a frozen WIP Council member";
constexpr auto INVALID_CHOICE = "invalid WIP ballot choice";
constexpr auto DUPLICATE_BALLOT = "account already voted on this WIP";
constexpr auto BEFORE_DEADLINE = "WIP deadline has not passed";
constexpr auto DECISION_READY = "WIP outcome is already finalizable";
constexpr auto INVALID_REASON = "WIP cancellation requires a bounded printable reason";

/// Restrict metadata to printable ASCII; content itself remains off-chain.
bool printable(const std::string& value) {
   return std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= ' ' && c <= '~'; });
}

/// Terminal records cannot be reopened, cancelled or recounted.
void check_open(const council::wip_row& row) {
   check(row.status == councl::wip_status::OPEN, CLOSED_WIP);
}

/// Bind user intent to the immutable proposal revision before any write.
void check_revision(const council::wip_row& row, const checksum256& content_hash) {
   check_open(row);
   check(content_hash == row.content_hash, WRONG_REVISION);
}
} // namespace

uint64_t council::openwip(std::string uri, checksum256 content_hash, uint64_t council_gen, wip_policy policy) {
   require_auth(get_self());
   check(uri.size() > HTTPS_PREFIX.size() && uri.size() <= councl::MAX_WIP_URI_BYTES &&
            uri.compare(0, HTTPS_PREFIX.size(), HTTPS_PREFIX) == 0 && printable(uri),
         INVALID_URI);
   check(content_hash != checksum256{}, EMPTY_HASH);
   check(policy.endorsement_seconds > 0 && policy.endorsement_seconds <= councl::MAX_TIME_SLOT_SEC &&
            policy.voting_seconds > 0 && policy.voting_seconds <= councl::MAX_TIME_SLOT_SEC,
         INVALID_WINDOW);

   // Selection is explicit governance approval for this proposal, not an automatic authority handover.
   // Completed election results survive reset; partial results cannot supply all 21 unique members.
   council_t elected(get_self(), council_gen);
   std::vector<name> members;
   for (uint8_t seat = 0; seat < councl::SEATS; ++seat) {
      const auto result = elected.try_get(index_key{seat});
      check(result.has_value() && result->member != name{}, INVALID_COUNCIL);
      check(std::find(members.begin(), members.end(), result->member) == members.end(), INVALID_COUNCIL);
      members.push_back(result->member);
   }

   // The canonical by-tier index visits at most 84 matching owners, plus one cap check. Snapshot
   // atomically, without staged-loading races or pinning election snapshots against their cleanup.
   const auto network_gen = roa_network_gen();
   roa::nodeowners_t owners(councl::ROA_ACCOUNT, network_gen);
   auto tier2 = owners.get_index<"bytier"_n>();
   const auto tier = magic_enum::enum_integer(councl::election_tier::T2);
   std::vector<name> endorsers;
   for (auto it = tier2.lower_bound(tier); it != tier2.end() && it->tier == tier; ++it) {
      check(endorsers.size() < sysiosystem::emissions::T2_MAX_NODE_OWNERS, INVALID_T2);
      endorsers.push_back(it->owner);
   }
   check(!endorsers.empty(), INVALID_T2);
   check(policy.minimum_endorsements > 0 && policy.minimum_endorsements <= endorsers.size() && policy.quorum > 0 &&
            policy.quorum <= members.size() && policy.minimum_yes > 0 && policy.minimum_yes <= members.size(),
         INVALID_POLICY);

   wipcounter_t counter(get_self());
   auto ids = counter.get_or_default(wip_counter{});
   check(ids.next_id > 0 && ids.next_id < std::numeric_limits<uint64_t>::max(), EXHAUSTED_IDS);
   const auto id = ids.next_id++;
   const auto now = current_time_point();
   const auto endorsement_end = now + seconds(policy.endorsement_seconds);
   wip_row row{.id = id,
               .uri = std::move(uri),
               .content_hash = content_hash,
               .network_gen = network_gen,
               .council_gen = council_gen,
               .policy = policy,
               .endorsers = std::move(endorsers),
               .members = std::move(members),
               .opened_at = now,
               .endorse_until = endorsement_end,
               .vote_until = endorsement_end + seconds(policy.voting_seconds)};
   wips_t(get_self()).emplace(RAM_PAYER, wip_key{id}, row);
   counter.set(ids, RAM_PAYER);
   return id;
}

void council::endorsewip(name owner, uint64_t proposal_id, checksum256 content_hash) {
   require_auth(owner);
   wips_t proposals(get_self());
   auto row = proposals.get(wip_key{proposal_id}, MISSING_WIP);
   check_revision(row, content_hash);
   check(current_time_point() < row.endorse_until, ENDORSEMENT_CLOSED);
   check(std::find(row.endorsers.begin(), row.endorsers.end(), owner) != row.endorsers.end(), NOT_ENDORSER);
   wipendorses_t endorsements(get_self(), proposal_id);
   const cand_key key{owner.value};
   check(!endorsements.try_get(key).has_value(), DUPLICATE_ENDORSEMENT);
   endorsements.emplace(RAM_PAYER, key, wip_endorsement{owner, content_hash, current_time_point()});
   ++row.endorsements;
   proposals.set(RAM_PAYER, wip_key{proposal_id}, row);
}

void council::votewip(name voter, uint64_t proposal_id, checksum256 content_hash, councl::wip_choice choice) {
   require_auth(voter);
   wips_t proposals(get_self());
   auto row = proposals.get(wip_key{proposal_id}, MISSING_WIP);
   check_revision(row, content_hash);
   const auto now = current_time_point();
   check(now >= row.endorse_until && now < row.vote_until, VOTING_CLOSED);
   check(row.endorsements >= row.policy.minimum_endorsements, ENDORSEMENTS_MISSING);
   check(std::find(row.members.begin(), row.members.end(), voter) != row.members.end(), NOT_MEMBER);
   check(magic_enum::enum_contains(choice), INVALID_CHOICE);
   wipballots_t ballots(get_self(), proposal_id);
   const cand_key key{voter.value};
   check(!ballots.try_get(key).has_value(), DUPLICATE_BALLOT);
   ballots.emplace(RAM_PAYER, key, wip_ballot{voter, content_hash, choice, now});
   switch (choice) {
   case councl::wip_choice::YES:
      ++row.yes;
      break;
   case councl::wip_choice::NO:
      ++row.no;
      break;
   case councl::wip_choice::ABSTAIN:
      ++row.abstain;
      break;
   }
   proposals.set(RAM_PAYER, wip_key{proposal_id}, row);
}

void council::settlewip(name caller, uint64_t proposal_id) {
   require_auth(caller);
   wips_t proposals(get_self());
   auto row = proposals.get(wip_key{proposal_id}, MISSING_WIP);
   check_open(row);
   const auto now = current_time_point();
   check(now >= row.endorse_until, BEFORE_DEADLINE);
   if (row.endorsements < row.policy.minimum_endorsements) {
      row.status = councl::wip_status::EXPIRED;
   } else {
      check(now >= row.vote_until, BEFORE_DEADLINE);
      const auto participation = row.yes + row.no + row.abstain;
      row.status = participation < row.policy.quorum   ? councl::wip_status::EXPIRED
                   : row.yes >= row.policy.minimum_yes ? councl::wip_status::PASSED
                                                       : councl::wip_status::REJECTED;
   }
   row.closed_at = now;
   proposals.set(RAM_PAYER, wip_key{proposal_id}, row);
}

void council::cancelwip(uint64_t proposal_id, std::string reason) {
   require_auth(get_self());
   check(!reason.empty() && reason.size() <= councl::MAX_WIP_REASON_BYTES && printable(reason), INVALID_REASON);
   wips_t proposals(get_self());
   auto row = proposals.get(wip_key{proposal_id}, MISSING_WIP);
   check_open(row);
   const auto now = current_time_point();
   check(now < row.vote_until && (now < row.endorse_until || row.endorsements >= row.policy.minimum_endorsements),
         DECISION_READY);
   row.status = councl::wip_status::CANCELLED;
   row.reason = std::move(reason);
   row.closed_at = current_time_point();
   proposals.set(RAM_PAYER, wip_key{proposal_id}, row);
}
} // namespace sysio
