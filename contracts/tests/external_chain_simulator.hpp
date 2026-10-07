#pragma once

#include "opp_envelope_oracle.hpp"
#include <sysio/opp/attestations/attestations.pb.h>
#include <fc/exception/exception.hpp>
#include <fc/slug_name.hpp>
#include <array>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <utility>

namespace sysio::testing::external {
using namespace sysio::opp;
using namespace sysio::opp::types;

/// Chain identifiers are deliberately unrelated to production assets. ChainKind is
/// retained because public-key encodings really are transport-family-specific.
struct asset_spec {
   const char* chain;
   const char* token;
   ChainKind kind;
   uint32_t id;
};
inline constexpr asset_spec First{"EC1", "NTA", ChainKind::CHAIN_KIND_EVM, 101};
inline constexpr asset_spec Second{"EC2", "NTB", ChainKind::CHAIN_KIND_SVM, 202};
inline constexpr std::array Assets{First, Second};
inline constexpr uint64_t Unit = 1'000'000'000;
inline constexpr uint32_t Precision = 9;
inline constexpr uint64_t EnvelopeTimestamp = 1'775'612'516'983ULL;
using attestation = std::pair<AttestationType, std::string>;

/// Test-only external custody and message source. It does NOT implement any
/// depot policy, bond adjudication, fee arithmetic, or token minting. Tests feed
/// these protobuf messages to production sysio.msgch, or its authenticated
/// downstream action boundary in focused contract suites. No sockets or daemons.
class chain {
public:
   explicit chain(asset_spec asset) : asset_(asset) {}
   const asset_spec& asset() const { return asset_; }
   uint64_t custody() const { return custody_; }
   uint64_t sequence() const { return sequence_; }
   uint64_t paid() const { return paid_; }
   size_t pending_count() const { return pending_.size(); }
   void freeze() { frozen_ = true; }
   void clear() { frozen_ = false; }

   /// Back a bootstrap import or repair a custody deficit without reporting yield.
   void donate(uint64_t amount) {
      FC_ASSERT(amount <= std::numeric_limits<uint64_t>::max() - custody_, "custody overflow");
      custody_ += amount;
   }
   /// Explicit fault injection, used to prove the depot notices an honest report
   /// of a shortfall. This is never an ordinary outpost operation.
   void lose(uint64_t amount) {
      FC_ASSERT(amount <= custody_, "custody underflow");
      custody_ -= amount;
   }
   attestation deposit(const std::vector<char>& public_key, uint64_t amount) {
      FC_ASSERT(amount > 0 && amount <= uint64_t(std::numeric_limits<int64_t>::max()), "invalid deposit amount");
      FC_ASSERT(sequence_ != std::numeric_limits<uint64_t>::max(), "sequence overflow");
      donate(amount);
      attestations::SyndicateLIQ message;
      message.set_chain_code(fc::slug_name{asset_.chain}.value);
      message.mutable_user()->set_kind(asset_.kind);
      message.mutable_user()->set_address(public_key.data(), public_key.size());
      message.mutable_amount()->set_token_code(fc::slug_name{asset_.token}.value);
      message.mutable_amount()->set_amount(amount);
      message.set_sequence(++sequence_);
      message.set_total_syndicated(custody_);
      return {AttestationType::ATTESTATION_TYPE_SYNDICATE_LIQ, message.SerializeAsString()};
   }
   attestation yield(uint64_t amount) {
      FC_ASSERT(amount > 0 && amount <= uint64_t(std::numeric_limits<int64_t>::max()), "invalid yield amount");
      FC_ASSERT(sequence_ != std::numeric_limits<uint64_t>::max(), "sequence overflow");
      FC_ASSERT(yield_epoch_ != std::numeric_limits<uint64_t>::max(), "yield epoch overflow");
      donate(amount);
      attestations::LIQYield message;
      message.set_chain_code(fc::slug_name{asset_.chain}.value);
      message.mutable_amount()->set_token_code(fc::slug_name{asset_.token}.value);
      message.mutable_amount()->set_amount(amount);
      message.set_sequence(++sequence_);
      message.set_epoch(++yield_epoch_);
      message.set_total_syndicated(custody_);
      return {AttestationType::ATTESTATION_TYPE_LIQ_YIELD, message.SerializeAsString()};
   }

   /// Build a canonical message chain. Calling this commits the simulator's
   /// transport cursor; fault/replay tests retain and redeliver the returned bytes.
   std::vector<char> envelope(uint32_t epoch, const std::vector<attestation>& entries) {
      Envelope result;
      result.set_epoch_index(epoch);
      result.set_epoch_envelope_index(1);
      result.set_epoch_timestamp(EnvelopeTimestamp);
      result.set_previous_envelope_hash(previous_envelope_);
      auto* message = result.add_messages();
      for (const auto& [type, data] : entries) {
         auto* entry = message->mutable_payload()->add_attestations();
         entry->set_type(type);
         entry->set_data(data);
         entry->set_data_size(data.size());
      }
      oracle::finalize_header(*message, previous_message_, EnvelopeTimestamp);
      previous_envelope_ = oracle::digest_bytes(oracle::epoch_digest(result));
      previous_message_ = message->header().message_id();
      const auto bytes = result.SerializeAsString();
      return {bytes.begin(), bytes.end()};
   }

   /// Consume a real depot-emitted return instruction. Replays cannot move
   /// custody twice; a frozen outpost retains a pending obligation until cleared.
   bool receive(const attestations::DesyndicateLIQ& message) {
      FC_ASSERT(message.chain_code() == fc::slug_name(asset_.chain).value, "wrong return chain");
      FC_ASSERT(message.amount().token_code() == fc::slug_name(asset_.token).value, "wrong return token");
      FC_ASSERT(message.request_id() != 0 && message.amount().amount() > 0, "invalid return");
      const auto id = message.request_id();
      const auto [seen, fresh] = received_.emplace(id, message.SerializeAsString());
      FC_ASSERT(fresh || seen->second == message.SerializeAsString(), "conflicting return replay");
      if (paid_requests_.contains(id)) return false;
      const auto amount = message.amount().amount();
      const auto [it, inserted] = pending_.emplace(id, amount);
      FC_ASSERT(inserted || it->second == amount, "conflicting return replay");
      return settle(id);
   }
   bool settle(uint64_t id) {
      const auto it = pending_.find(id);
      if (frozen_ || it == pending_.end()) return false;
      FC_ASSERT(it->second <= custody_, "insufficient external custody");
      custody_ -= it->second;
      paid_ += it->second;
      pending_.erase(it);
      paid_requests_.insert(id);
      return true;
   }
private:
   asset_spec asset_;
   uint64_t custody_ = 0, sequence_ = 0, yield_epoch_ = 0, paid_ = 0;
   bool frozen_ = false;
   std::map<uint64_t, uint64_t> pending_;
   std::set<uint64_t> paid_requests_;
   std::map<uint64_t, std::string> received_;
   std::string previous_envelope_, previous_message_;
};
} // namespace sysio::testing::external
