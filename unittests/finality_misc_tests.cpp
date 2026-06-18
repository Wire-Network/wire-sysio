#include <sysio/chain/qc.hpp>
#include <sysio/chain/types.hpp>
#include <sysio/chain/block_header.hpp>

#include <fc/exception/exception.hpp>
#include <fc/crypto/bls_private_key.hpp>
#include <fc/crypto/bls_utils.hpp>

#include <boost/test/unit_test.hpp>

// -----------------------------------------------------------------------------
//            Allow boost to print `aggregating_qc_sig_t::state_t`
// -----------------------------------------------------------------------------
namespace std {
   using state_t = sysio::chain::aggregating_qc_sig_t::state_t;
   std::ostream& operator<<(std::ostream& os, state_t s)
   {
      switch(s) {
      case state_t::unrestricted:   os << "unrestricted"; break;
      case state_t::restricted:     os << "restricted"; break;
      case state_t::weak_achieved:  os << "weak_achieved"; break;
      case state_t::weak_final:     os << "weak_final"; break;
      case state_t::strong:         os << "strong"; break;
      }
      return os;
   }
}

BOOST_AUTO_TEST_SUITE(finality_misc_tests)

BOOST_AUTO_TEST_CASE(qc_state_transitions) try {
   using namespace sysio::chain;
   using namespace fc::crypto::bls;
   using state_t = aggregating_qc_sig_t::state_t;

   digest_type d(fc::sha256("0000000000000000000000000000001"));
   std::vector<uint8_t> digest(d.data(), d.data() + d.data_size());

   std::vector<bls_private_key> sk {
      bls_private_key("PVT_BLS_0d8dsux83r42Qg8CHgAqIuSsn9AV-QdCzx3tPj0K8yOJA_qb"),
      bls_private_key("PVT_BLS_Wfs3KzfTI2P5F85PnoHXLnmYgSbp-XpebIdS6BUCHXOKmKXK"),
      bls_private_key("PVT_BLS_74crPc__6BlpoQGvWjkHmUdzcDKh8QaiN_GtU4SD0QAi4BHY"),
      bls_private_key("PVT_BLS_foNjZTu0k6qM5ftIrqC5G_sim1Rg7wq3cRUaJGvNtm2rM89K"),
      bls_private_key("PVT_BLS_FWK1sk_DJnoxNvUNhwvJAYJFcQAFtt_mCtdQCUPQ4jN1K7eT"),
      bls_private_key("PVT_BLS_tNAkC5MnI-fjHWSX7la1CPC2GIYgzW5TBfuKFPagmwVVsOeW")
   };

   std::vector<bls_public_key> pubkey;
   pubkey.reserve(sk.size());
   for (const auto& k : sk)
      pubkey.push_back(k.get_public_key());

   auto weak_vote = [&](aggregating_qc_sig_t& qc, const std::vector<uint8_t>& digest_to_sign, size_t index, uint64_t weight) {
      return qc.add_vote(0, 0, false, index, sk[index].sign_raw(digest_to_sign), weight);
   };

   auto strong_vote = [&](aggregating_qc_sig_t& qc, const std::vector<uint8_t>& digest_to_sign, size_t index, uint64_t weight) {
      return qc.add_vote(0, 0, true, index, sk[index].sign_raw(digest_to_sign), weight);
   };

   constexpr uint64_t weight = 1;

   {
      constexpr uint64_t quorum = 1;
      constexpr uint64_t max_weak_sum_before_weak_final = 1;
      aggregating_qc_sig_t qc(2, quorum, max_weak_sum_before_weak_final); // 2 finalizers
      BOOST_CHECK_EQUAL(qc.state(), state_t::unrestricted);

      // add one weak vote
      // -----------------
      weak_vote(qc, digest, 0, weight);
      BOOST_CHECK_EQUAL(qc.state(), state_t::weak_achieved);
      BOOST_CHECK(qc.is_quorum_met());

      // add duplicate weak vote
      // -----------------------
      auto ok = weak_vote(qc, digest, 0, weight);
      BOOST_CHECK(ok != vote_result_t::success); // vote was a duplicate
      BOOST_CHECK_EQUAL(qc.state(), state_t::weak_achieved);
      BOOST_CHECK(qc.is_quorum_met());

      // add another weak vote
      // ---------------------
      weak_vote(qc, digest, 1, weight);
      BOOST_CHECK_EQUAL(qc.state(), state_t::weak_final);
   }

   {
      constexpr uint64_t quorum = 1;
      constexpr uint64_t max_weak_sum_before_weak_final = 1;
      aggregating_qc_sig_t qc(2, quorum, max_weak_sum_before_weak_final); // 2 finalizers
      BOOST_CHECK_EQUAL(qc.state(), state_t::unrestricted);

      // add a weak vote
      // ---------------
      weak_vote(qc, digest, 0, weight);
      BOOST_CHECK_EQUAL(qc.state(), state_t::weak_achieved);
      BOOST_CHECK(qc.is_quorum_met());

      // add a strong vote
      // -----------------
      strong_vote(qc, digest, 1, weight);
      BOOST_CHECK_EQUAL(qc.state(), state_t::strong);
      BOOST_CHECK(qc.is_quorum_met());
   }

   {
      constexpr uint64_t quorum = 1;
      constexpr uint64_t max_weak_sum_before_weak_final = 1;
      aggregating_qc_sig_t qc(2, quorum, max_weak_sum_before_weak_final); // 2 finalizers, weight_sum_minus_quorum = 1
      BOOST_CHECK_EQUAL(qc.state(), state_t::unrestricted);

      // add a strong vote
      // -----------------
      strong_vote(qc, digest, 1, weight);
      BOOST_CHECK_EQUAL(qc.state(), state_t::strong);
      BOOST_CHECK(qc.is_quorum_met());

      // add a strong vote
      // -----------------
      strong_vote(qc, digest, 1, weight);
      BOOST_CHECK_EQUAL(qc.state(), state_t::strong);
      BOOST_CHECK(qc.is_quorum_met());
   }

   {
      constexpr uint64_t quorum = 2;
      constexpr uint64_t max_weak_sum_before_weak_final = 1;
      aggregating_qc_sig_t qc(3, quorum, max_weak_sum_before_weak_final); // 3 finalizers

      // add a weak vote
      // ---------------
      weak_vote(qc, digest, 0, weight);
      BOOST_CHECK_EQUAL(qc.state(), state_t::unrestricted);
      BOOST_CHECK(!qc.is_quorum_met());

      // add a strong vote
      // -----------------
      strong_vote(qc, digest, 1, weight);
      BOOST_CHECK_EQUAL(qc.state(), state_t::weak_achieved);
      BOOST_CHECK(qc.is_quorum_met());

      {
         aggregating_qc_sig_t qc2(std::move(qc));

         // add a weak vote
         // ---------------
         weak_vote(qc2, digest, 2, weight);
         BOOST_CHECK_EQUAL(qc2.state(), state_t::weak_final);
         BOOST_CHECK(qc2.is_quorum_met());
      }
   }

   {
      constexpr uint64_t quorum = 2;
      constexpr uint64_t max_weak_sum_before_weak_final = 1;
      aggregating_qc_sig_t qc(3, quorum, max_weak_sum_before_weak_final); // 3 finalizers, quorum = 2

      // add a weak vote
      // ---------------
      weak_vote(qc, digest, 0, weight);
      BOOST_CHECK_EQUAL(qc.state(), state_t::unrestricted);
      BOOST_CHECK(!qc.is_quorum_met());

      // add a strong vote
      // -----------------
      strong_vote(qc, digest, 1, weight);
      BOOST_CHECK_EQUAL(qc.state(), state_t::weak_achieved);
      BOOST_CHECK(qc.is_quorum_met());

      {
         aggregating_qc_sig_t qc2(std::move(qc));

         // add a strong vote
         // -----------------
         strong_vote(qc2, digest, 2, weight);
         BOOST_CHECK_EQUAL(qc2.state(), state_t::strong);
         BOOST_CHECK(qc2.is_quorum_met());
      }
   }

   {
      constexpr uint64_t quorum = 2;
      constexpr uint64_t max_weak_sum_before_weak_final = 1;
      aggregating_qc_sig_t qc(3, quorum, max_weak_sum_before_weak_final); // 3 finalizers, quorum = 2

      // add a weak vote
      // ---------------
      weak_vote(qc, digest, 0, weight);
      BOOST_CHECK_EQUAL(qc.state(), state_t::unrestricted);
      BOOST_CHECK(!qc.is_quorum_met());

      // add a weak vote
      // ---------------
      weak_vote(qc, digest, 1, weight);
      BOOST_CHECK_EQUAL(qc.state(), state_t::weak_final);
      BOOST_CHECK(qc.is_quorum_met());

      {
         aggregating_qc_sig_t qc2(std::move(qc));

         // add a weak vote
         // ---------------
         weak_vote(qc2, digest, 2, weight);
         BOOST_CHECK_EQUAL(qc2.state(), state_t::weak_final);
         BOOST_CHECK(qc2.is_quorum_met());
      }
   }

   {
      constexpr uint64_t quorum = 2;
      constexpr uint64_t max_weak_sum_before_weak_final = 1;
      aggregating_qc_sig_t qc(3, quorum, max_weak_sum_before_weak_final); // 3 finalizers, quorum = 2

      // add a weak vote
      // ---------------
      weak_vote(qc, digest, 0, weight);
      BOOST_CHECK_EQUAL(qc.state(), state_t::unrestricted);
      BOOST_CHECK(!qc.is_quorum_met());

      // add a weak vote
      // ---------------
      weak_vote(qc, digest, 1, weight);
      BOOST_CHECK_EQUAL(qc.state(), state_t::weak_final);
      BOOST_CHECK(qc.is_quorum_met());

      {
         aggregating_qc_sig_t qc2(std::move(qc));

         // add a strong vote
         // -----------------
         strong_vote(qc2, digest, 2, weight);
         BOOST_CHECK_EQUAL(qc2.state(), state_t::weak_final);
         BOOST_CHECK(qc2.is_quorum_met());
      }
   }

   // -------------------------------------------------------------------------
   //  weak_achieved-arm boundary at weak_sum == max_weak_sum_before_weak_final
   // -------------------------------------------------------------------------
   // weak_final means a strong QC can no longer form. The maximum still-achievable
   // strong sum is (weight_sum - weak_sum), so at weak_sum == max_weak_sum_before_weak_final
   // (== weight_sum - threshold) that maximum equals exactly the threshold: a strong QC is
   // STILL reachable, and the state must remain weak_achieved (not weak_final). The
   // add_weak_vote weak_achieved arm must therefore use strict '>' (matching the
   // unrestricted/restricted arm), not '>='.
   //
   // The cases above all use max_weak_sum_before_weak_final == 1, where weak_achieved can
   // only be entered once weak_sum >= 1 == max, so the weak vote that reaches the boundary
   // is also the one that enters weak_achieved (via the '>' unrestricted arm) and the
   // weak_achieved arm is never exercised AT the boundary. These cases use max == 3 so
   // weak_achieved is entered (strong-assisted) while weak_sum < max, then a weak vote
   // drives weak_sum to exactly max through the weak_achieved arm.
   {
      // weight_sum 8, threshold 5  ->  quorum 5, max_weak_sum_before_weak_final 3
      constexpr uint64_t quorum = 5;
      constexpr uint64_t max_weak_sum_before_weak_final = 3;
      aggregating_qc_sig_t qc(6, quorum, max_weak_sum_before_weak_final);

      strong_vote(qc, digest, 0, 3); // strong_sum = 3  (< quorum)
      BOOST_CHECK_EQUAL(qc.state(), state_t::unrestricted);

      weak_vote(qc, digest, 1, 1);   // weak_sum = 1, weak+strong = 4  (< quorum)
      BOOST_CHECK_EQUAL(qc.state(), state_t::unrestricted);

      weak_vote(qc, digest, 2, 1);   // weak_sum = 2, weak+strong = 5  -> weak_achieved (weak_sum < max)
      BOOST_CHECK_EQUAL(qc.state(), state_t::weak_achieved);

      weak_vote(qc, digest, 3, 1);   // weak_sum = 3 == max: strong still reachable -> stays weak_achieved
      BOOST_CHECK_EQUAL(qc.state(), state_t::weak_achieved); // would be weak_final with the '>=' off-by-one

      weak_vote(qc, digest, 4, 1);   // weak_sum = 4 > max -> weak_final
      BOOST_CHECK_EQUAL(qc.state(), state_t::weak_final);
   }

   {
      // From weak_sum == max the node must still be able to form a STRONG QC if the
      // remaining (exactly-threshold) weight votes strong. The '>=' off-by-one would have
      // latched weak_final at the boundary, so this strong vote could never upgrade it —
      // needlessly downgrading the QC and delaying 2-chain finality.
      constexpr uint64_t quorum = 5;
      constexpr uint64_t max_weak_sum_before_weak_final = 3;
      aggregating_qc_sig_t qc(6, quorum, max_weak_sum_before_weak_final);

      strong_vote(qc, digest, 0, 3); // strong_sum = 3
      weak_vote(qc, digest, 1, 1);   // weak_sum = 1
      weak_vote(qc, digest, 2, 1);   // weak_sum = 2 -> weak_achieved
      weak_vote(qc, digest, 3, 1);   // weak_sum = 3 == max -> stays weak_achieved (fixed)
      BOOST_CHECK_EQUAL(qc.state(), state_t::weak_achieved);

      strong_vote(qc, digest, 4, 2); // strong_sum = 5 == quorum -> strong
      BOOST_CHECK_EQUAL(qc.state(), state_t::strong);
      BOOST_CHECK(qc.is_quorum_met());
   }

   // weak_final is correctly terminal: once weak_sum > max_weak_sum_before_weak_final the remaining
   // unvoted weight (weight_sum - weak_sum) is below the threshold, so a strong QC is genuinely
   // unreachable and the state must stay weak_final even after every remaining finalizer votes
   // strong. This is the complement of the weak_achieved-boundary case above (at weak_sum == max a
   // strong QC IS still reachable); together they pin the exact semantics on both sides of the
   // boundary. Correct under both '>' and '>=' — coverage of terminal behavior, not the off-by-one.
   {
      constexpr uint64_t quorum = 5;
      constexpr uint64_t max_weak_sum_before_weak_final = 3; // weight_sum 8, threshold 5
      aggregating_qc_sig_t qc(6, quorum, max_weak_sum_before_weak_final);

      strong_vote(qc, digest, 0, 3); // strong_sum = 3
      weak_vote(qc, digest, 1, 1);   // weak_sum = 1
      weak_vote(qc, digest, 2, 1);   // weak_sum = 2 -> weak_achieved
      weak_vote(qc, digest, 3, 1);   // weak_sum = 3 == max -> still weak_achieved
      weak_vote(qc, digest, 4, 1);   // weak_sum = 4 > max  -> weak_final
      BOOST_CHECK_EQUAL(qc.state(), state_t::weak_final);

      // All remaining weight (1) votes strong: strong_sum reaches 4 < quorum 5, so strong cannot
      // form and the state stays weak_final (a strong vote into weak_final is a no-op).
      strong_vote(qc, digest, 5, 1); // strong_sum = 4 (< quorum)
      BOOST_CHECK_EQUAL(qc.state(), state_t::weak_final);
   }

   // restricted-arm path to weak_final: weak votes that push weak_sum past max BEFORE quorum take
   // the state unrestricted -> restricted (that arm already uses strict '>'), and reaching quorum
   // from restricted goes straight to weak_final. Exercises the restricted intermediate state and
   // the unrestricted/restricted boundary at weak_sum == max (which must NOT yet be restricted).
   {
      constexpr uint64_t quorum = 5;
      constexpr uint64_t max_weak_sum_before_weak_final = 3; // weight_sum 8, threshold 5
      aggregating_qc_sig_t qc(6, quorum, max_weak_sum_before_weak_final);

      weak_vote(qc, digest, 0, 1);   // weak_sum = 1 (< quorum, < max)
      weak_vote(qc, digest, 1, 1);   // weak_sum = 2
      weak_vote(qc, digest, 2, 1);   // weak_sum = 3 == max: not > max, quorum not met -> unrestricted
      BOOST_CHECK_EQUAL(qc.state(), state_t::unrestricted);
      weak_vote(qc, digest, 3, 1);   // weak_sum = 4 > max, quorum not met -> restricted
      BOOST_CHECK_EQUAL(qc.state(), state_t::restricted);
      weak_vote(qc, digest, 4, 1);   // weak_sum = 5 == quorum and > max -> weak_final
      BOOST_CHECK_EQUAL(qc.state(), state_t::weak_final);
   }

} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()
