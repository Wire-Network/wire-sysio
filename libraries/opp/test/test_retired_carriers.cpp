/**
 * @file test_retired_carriers.cpp
 * @brief Removed lifecycle payloads stay out of generated APIs while opaque
 *        attestations round-trip without hiding the next live LIQ message.
 */
#include <array>
#include <string>

#include <boost/test/unit_test.hpp>
#include <google/protobuf/descriptor.h>
#include <fc/slug_name.hpp>
#include <sysio/opp/opp.hpp>
#include <sysio/opp/opp.pb.h>

namespace {

/// Removed lifecycle discriminants, plus an undeclared control.
constexpr std::array removed_wire_types{3001, 3002, 3004, 3006, 43520, 60928, 60930, 60932,
                                        60934, 60944, 60945, 60950, 60951, 60952, 60953,
                                        60955, 60956, 60958, 60959, 60960, 60961, 60962,
                                        65000};
constexpr auto type_field = "type";
using fc::slug_name_literals::operator""_s;
constexpr auto chain_code = "SOLANA"_s.value;
constexpr auto token_code = "LIQSOL"_s.value;
constexpr int64_t reported_yield = 1'000'000;
constexpr uint64_t report_sequence = 7;
constexpr auto malformed_payload = "\x0a";

/// Removed `AttestationType` value names; none may reappear in the generated enum.
constexpr std::array removed_attestation_type_names{
   "ATTESTATION_TYPE_STAKE",
   "ATTESTATION_TYPE_UNSTAKE",
   "ATTESTATION_TYPE_PRETOKEN_PURCHASE",
   "ATTESTATION_TYPE_PRETOKEN_YIELD",
   "ATTESTATION_TYPE_RESERVE_BALANCE_SHEET",
   "ATTESTATION_TYPE_STAKE_UPDATE",
   "ATTESTATION_TYPE_WIRE_TOKEN_PURCHASE",
   "ATTESTATION_TYPE_SWAP_REQUEST",
   "ATTESTATION_TYPE_SWAP_REMIT",
   "ATTESTATION_TYPE_STAKE_RESULT",
   "ATTESTATION_TYPE_ATTESTATION_PROCESSING_ERROR",
   "ATTESTATION_TYPE_UNDERWRITE_INTENT_COMMIT",
   "ATTESTATION_TYPE_SWAP_REVERT",
   "ATTESTATION_TYPE_DEPOSIT_REVERT",
   "ATTESTATION_TYPE_RESERVE_CREATE_CANCEL",
   "ATTESTATION_TYPE_RESERVE_CREATE_CANCELLED",
   "ATTESTATION_TYPE_RESERVE_READY",
   "ATTESTATION_TYPE_EMISSIONS_BLOCKED",
   "ATTESTATION_TYPE_CHALLENGE_RESPONSE",
   "ATTESTATION_TYPE_CHALLENGE_REQUEST"};
constexpr auto attestation_type_enum = "sysio.opp.types.AttestationType";

} // namespace

BOOST_AUTO_TEST_SUITE(retired_opp_carriers)

/// Generated descriptors must expose live custody messages, without the removed APIs.
BOOST_AUTO_TEST_CASE(generated_api_omits_retired_lifecycle_messages) {
   const auto* pool = google::protobuf::DescriptorPool::generated_pool();
   BOOST_REQUIRE(pool->FindMessageTypeByName("sysio.opp.attestations.DesyndicateLIQ"));
   BOOST_REQUIRE(pool->FindMessageTypeByName("sysio.opp.attestations.OperatorAction"));
   BOOST_REQUIRE(pool->FindMessageTypeByName("sysio.opp.attestations.NodeOwnerRegistration"));
   BOOST_REQUIRE(pool->FindEnumTypeByName("sysio.opp.types.EmissionsBlockReason"));
   for (const auto* name : {
           "sysio.opp.types.Reserve", "sysio.opp.types.ReserveAmount",
           "sysio.opp.attestations.ReserveBalanceSheet",
           "sysio.opp.attestations.ReserveDisbursement",
           "sysio.opp.attestations.ReserveCreateCancel",
           "sysio.opp.attestations.ReserveCreateCancelled",
           "sysio.opp.attestations.ReserveReady",
           "sysio.opp.attestations.EmissionsBlocked",
           "sysio.opp.attestations.NodeOwnerReg",
           "sysio.opp.attestations.ProtocolState",
           "sysio.opp.attestations.PretokenPurchase",
           "sysio.opp.attestations.PretokenYield",
           "sysio.opp.attestations.WireTokenPurchase",
           "sysio.opp.attestations.SwapRequest",
           "sysio.opp.attestations.UnderwriteIntentCommit",
           "sysio.opp.attestations.SwapRemit",
           "sysio.opp.attestations.SwapRevert",
           "sysio.opp.attestations.DepositRevert",
           "sysio.opp.attestations.PretokenStakeChange",
           "sysio.opp.attestations.StakeUpdate",
           "sysio.opp.attestations.StakeResult",
           "sysio.opp.attestations.AttestationProcessingError",
           "sysio.opp.attestations.ChallengeRequest",
           "sysio.opp.attestations.ChallengeOperatorHash",
           "sysio.opp.types.ChainSignature",
           "sysio.opp.types.WirePermission",
           "sysio.opp.types.Chain",
           "sysio.opp.types.Token",
           "sysio.opp.types.ChainToken",
           "sysio.opp.debugging.DebugEnvelopeDataRecord"}) {
      BOOST_CHECK_MESSAGE(pool->FindMessageTypeByName(name) == nullptr, name);
   }
   for (const auto* name : {"sysio.opp.types.ReserveStatus",
                            "sysio.opp.types.UnderwriteRequestStatus",
                            "sysio.opp.types.UnderwriteStatus",
                            "sysio.opp.types.StakeStatus",
                            "sysio.opp.types.ChainKeyType",
                            "sysio.opp.types.ChainRequestStatus",
                            "sysio.opp.types.ChallengeStatus"}) {
      BOOST_CHECK_MESSAGE(pool->FindEnumTypeByName(name) == nullptr, name);
   }
}

/// The generated `AttestationType` must define neither a removed value name nor a removed number.
BOOST_AUTO_TEST_CASE(generated_attestation_type_omits_removed_values) {
   const auto* attestation_types =
      google::protobuf::DescriptorPool::generated_pool()->FindEnumTypeByName(attestation_type_enum);
   BOOST_REQUIRE(attestation_types);
   BOOST_REQUIRE(attestation_types->FindValueByName("ATTESTATION_TYPE_LIQ_YIELD"));
   for (const auto* name : removed_attestation_type_names) {
      BOOST_CHECK_MESSAGE(attestation_types->FindValueByName(name) == nullptr, name);
   }
   for (const auto raw_type : removed_wire_types) {
      BOOST_CHECK_MESSAGE(attestation_types->FindValueByNumber(raw_type) == nullptr, raw_type);
   }
}

/// Unknown wire discriminants and malformed opaque data must not corrupt a following live entry.
BOOST_AUTO_TEST_CASE(opaque_removed_entries_preserve_following_liq_attestation) {
   sysio::opp::Envelope source;
   auto* payload = source.add_messages()->mutable_payload();
   for (const auto raw_type : removed_wire_types) {
      auto* entry = payload->add_attestations();
      const auto* field = entry->GetDescriptor()->FindFieldByName(type_field);
      BOOST_REQUIRE(field);
      entry->GetReflection()->SetEnumValue(entry, field, raw_type);
      entry->set_data(malformed_payload);
      entry->set_data_size(entry->data().size());
   }
   sysio::opp::attestations::LIQYield report;
   report.set_chain_code(chain_code);
   report.mutable_amount()->set_token_code(token_code);
   report.mutable_amount()->set_amount(reported_yield);
   report.set_sequence(report_sequence);
   const auto liq_payload = report.SerializeAsString();
   auto* live = payload->add_attestations();
   live->set_type(sysio::opp::types::ATTESTATION_TYPE_LIQ_YIELD);
   live->set_data(liq_payload);
   live->set_data_size(live->data().size());

   sysio::opp::Envelope decoded;
   BOOST_REQUIRE(decoded.ParseFromString(source.SerializeAsString()));
   const auto& entries = decoded.messages(0).payload().attestations();
   BOOST_REQUIRE_EQUAL(entries.size(), removed_wire_types.size() + 1);
   for (size_t index = 0; index < removed_wire_types.size(); ++index) {
      const auto& entry = entries.Get(index);
      BOOST_CHECK_EQUAL(entry.GetReflection()->GetEnumValue(
         entry, entry.GetDescriptor()->FindFieldByName(type_field)), removed_wire_types[index]);
      BOOST_CHECK_EQUAL(entry.data(), malformed_payload);
   }
   BOOST_CHECK_EQUAL(entries.rbegin()->type(), sysio::opp::types::ATTESTATION_TYPE_LIQ_YIELD);
   BOOST_CHECK_EQUAL(entries.rbegin()->data(), liq_payload);
   sysio::opp::attestations::LIQYield decoded_report;
   BOOST_REQUIRE(decoded_report.ParseFromString(entries.rbegin()->data()));
   BOOST_CHECK_EQUAL(decoded_report.chain_code(), chain_code);
   BOOST_CHECK_EQUAL(decoded_report.amount().token_code(), token_code);
   BOOST_CHECK_EQUAL(decoded_report.amount().amount(), reported_yield);
   BOOST_CHECK_EQUAL(decoded_report.sequence(), report_sequence);
}

BOOST_AUTO_TEST_SUITE_END()
