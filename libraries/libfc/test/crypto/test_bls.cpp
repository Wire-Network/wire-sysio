#include <boost/test/unit_test.hpp>

#include <bls12-381/bls12-381.hpp>
#include <fc/exception/exception.hpp>

#include <random>

using namespace std;
using namespace bls12_381;

namespace {

/// Seed of the generator the pairing tests draw their points from.
constexpr uint64_t pairing_seed = 0x5eed;

/// Pair counts the pairing yield tests run with.
constexpr array<size_t, 5> pairing_sizes = {1, 2, 7, 20, 33};

/// Thrown by a yield to stop a pairing computation.
struct yield_stop {};

/// `count` pairs of random multiples of the G1 and G2 generators, as pairing::add_pair stores them.
vector<tuple<g1, g2>> random_pairs(size_t count, mt19937_64& rng) {
   vector<tuple<g1, g2>> pairs;
   for (size_t i = 0; i < count; ++i) {
      const array<uint64_t, 4> s1 = {rng(), rng(), rng(), rng()};
      const array<uint64_t, 4> s2 = {rng(), rng(), rng(), rng()};
      pairing::add_pair(pairs, g1::one().scale(s1), g2::one().scale(s2));
   }
   return pairs;
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(bls)

// sample seeds
vector<uint8_t> seed_1 = { 0, 50, 6, 244, 24, 199, 1, 25, 52, 88, 192, 19, 18, 12, 89, 6, 220, 18, 102, 58, 209, 82, 12, 62, 89, 110, 182, 9, 44, 20, 254, 22};
vector<uint8_t> seed_2 = { 6, 51, 22, 89, 11, 15, 4, 61, 127, 241, 79, 26, 88, 52, 1, 6, 18, 79, 10, 8, 36, 182, 154, 35, 75, 156, 215, 41, 29, 90, 125, 233};
vector<uint8_t> seed_3 = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12};
// sample messages
vector<uint8_t> message_1 = { 51, 23, 56, 93, 212, 129, 128, 27, 251, 12, 42, 129, 210, 9, 34, 98};
vector<uint8_t> message_2 = { 16, 38, 54, 125, 71, 214, 217, 78, 73, 23, 127, 235, 8, 94, 41, 53};
vector<uint8_t> message_3 = { 12, 4, 1, 64, 127, 86, 2, 8, 145, 25, 27, 5, 88, 4, 42, 58};

// test a single key signature + verification
BOOST_AUTO_TEST_CASE(bls_sig_verify) try {
    array<uint64_t, 4> sk = secret_key(seed_1);
    g1 pk = public_key(sk);
    g2 signature = sign(sk, message_1);

    bool ok = verify(pk, message_1, signature);
    BOOST_CHECK_EQUAL(ok, true);
} FC_LOG_AND_RETHROW();

// test serialization / deserialization of private key, public key and signature
BOOST_AUTO_TEST_CASE(bls_serialization_test) try {
    array<uint64_t, 4> sk = secret_key(seed_1);
    g1 pk = public_key(sk);
    g2 signature = sign(sk, message_1);

    const array<uint8_t, 144> pk_string = pk.toJacobianBytesBE();
    const array<uint8_t, 288> signature_string = signature.toJacobianBytesBE();
    cout << bytesToHex<144>(pk_string) <<  std::endl;
    cout << bytesToHex<288>(signature_string) << std::endl;

    g1 pk2 = g1::fromJacobianBytesBE(pk_string).value();
    g2 signature2 = g2::fromJacobianBytesBE(signature_string).value();
    bool ok = verify(pk2, message_1, signature2);
    BOOST_CHECK_EQUAL(ok, true);
} FC_LOG_AND_RETHROW();

// test public keys + signatures aggregation + verification
BOOST_AUTO_TEST_CASE(bls_agg_sig_verify) try {
    array<uint64_t, 4> sk1 = secret_key(seed_1);
    g1 pk1 = public_key(sk1);
    g2 sig1 = sign(sk1, message_1);

    array<uint64_t, 4> sk2 = secret_key(seed_2);
    g1 pk2 = public_key(sk2);
    g2 sig2 = sign(sk2, message_2);

    g2 aggSig = aggregate_signatures(vector<g2> {sig1, sig2});

    bool ok = aggregate_verify(vector<g1>{pk1, pk2}, vector<vector<uint8_t>>{message_1, message_2}, aggSig);
    BOOST_CHECK_EQUAL(ok, true);
} FC_LOG_AND_RETHROW();

// test signature aggregation + aggregate tree verification
BOOST_AUTO_TEST_CASE(bls_agg_tree_verify) try {
    array<uint64_t, 4> sk1 = secret_key(seed_1);
    g1 pk1 = public_key(sk1);
    g2 sig1 = sign(sk1, message_1);

    array<uint64_t, 4> sk2 = secret_key(seed_2);
    g1 pk2 = public_key(sk2);
    g2 sig2 = sign(sk2, message_2);

    g2 aggSig = aggregate_signatures(vector<g2> {sig1, sig2});

    array<uint64_t, 4> sk3 = secret_key(seed_3);
    g1 pk3 = public_key(sk3);
    g2 sig3 = sign(sk3, message_3);

    g2 aggSigFinal = aggregate_signatures(vector<g2> {aggSig, sig3});

    bool ok = aggregate_verify(vector<g1>{pk1, pk2, pk3}, vector<vector<uint8_t>>{message_1, message_2, message_3}, aggSigFinal);
    BOOST_CHECK_EQUAL(ok, true);
} FC_LOG_AND_RETHROW();

// test public key aggregation
BOOST_AUTO_TEST_CASE(bls_agg_pk_verify) try {
    array<uint64_t, 4> sk1 = secret_key(seed_1);
    g1 pk1 = public_key(sk1);
    g2 sig1 = sign(sk1, message_1);

    array<uint64_t, 4> sk2 = secret_key(seed_2);
    g1 pk2 = public_key(sk2);
    g2 sig2 = sign(sk2, message_1);

    array<uint64_t, 4> sk3 = secret_key(seed_3);
    g1 pk3 = public_key(sk3);
    g2 sig3 = sign(sk3, message_1);

    g2 sigAgg = aggregate_signatures(vector<g2> {sig1, sig2, sig3});
    g1 pkAgg = aggregate_public_keys(vector<g1> {pk1, pk2, pk3});

    bool ok = verify(pkAgg, message_1, sigAgg);
    BOOST_CHECK_EQUAL(ok, true);
} FC_LOG_AND_RETHROW();

// test wrong key and wrong signature
BOOST_AUTO_TEST_CASE(bls_bad_sig_verify) try {
    array<uint64_t, 4> sk1 = secret_key(seed_1);
    g1 pk1 = public_key(sk1);
    g2 sig1 = sign(sk1, message_1);

    array<uint64_t, 4> sk2 = secret_key(seed_2);
    g1 pk2 = public_key(sk2);
    g2 sig2 = sign(sk2, message_1);

    bool ok1 = verify(pk1, message_1, sig2); // wrong signature
    bool ok2 = verify(pk2, message_1, sig1); // wrong key
    BOOST_CHECK_EQUAL(ok1, false);
    BOOST_CHECK_EQUAL(ok2, false);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(bls_pop_verify) try {
    array<uint64_t, 4> sk1 = secret_key(seed_1);
    g1 pk1 = public_key(sk1);
    g2 sig1 = sign(sk1, message_1);

    array<uint64_t, 4> sk2 = secret_key(seed_2);
    g1 pk2 = public_key(sk2);
    g2 sig2 = sign(sk2, message_1);

    g2 aggsig = aggregate_signatures(vector<g2> {sig1, sig2});
    bool ok = pop_fast_aggregate_verify(vector<g1>{pk1, pk2}, message_1, aggsig);
    BOOST_CHECK_EQUAL(ok, true);
} FC_LOG_AND_RETHROW();

/// The pairing calls its yield at least once per pair, and the yield leaves the result unchanged.
BOOST_AUTO_TEST_CASE(pairing_yields_per_pair) try {
   mt19937_64 rng(pairing_seed);
   for (const size_t size : pairing_sizes) {
      BOOST_TEST_CONTEXT("pairs " << size) {
         const vector<tuple<g1, g2>> pairs = random_pairs(size, rng);
         BOOST_REQUIRE_EQUAL(pairs.size(), size);
         size_t yields = 0;
         const fp12 yielding = pairing::calculate(pairs, [&]() { ++yields; });
         BOOST_CHECK_GE(yields, size);
         BOOST_CHECK(yielding.equal(pairing::calculate(pairs)));
      }
   }
} FC_LOG_AND_RETHROW();

/// What the yield throws leaves the pairing wherever the yield is called: at the first call, after the line
/// coefficients of the last pair, at the first call after them, and at the last call.
BOOST_AUTO_TEST_CASE(pairing_yield_exception_propagates) try {
   mt19937_64 rng(pairing_seed);
   for (const size_t size : pairing_sizes) {
      const vector<tuple<g1, g2>> pairs = random_pairs(size, rng);
      size_t yields = 0;
      pairing::calculate(pairs, [&]() { ++yields; });
      for (const size_t stop_at : {size_t{1}, size, size + 1, yields}) {
         BOOST_TEST_CONTEXT("pairs " << size << ", stop at yield " << stop_at << " of " << yields) {
            size_t calls = 0;
            const auto stop = [&]() {
               if (++calls == stop_at)
                  throw yield_stop{};
            };
            BOOST_CHECK_THROW(pairing::calculate(pairs, stop), yield_stop);
            BOOST_CHECK_EQUAL(calls, stop_at);
         }
      }
   }
} FC_LOG_AND_RETHROW();

/// The Miller loop and the pairing of no pairs are one, and call no yield.
BOOST_AUTO_TEST_CASE(pairing_of_no_pairs) try {
   size_t yields = 0;
   const auto count = [&]() { ++yields; };
   BOOST_CHECK(pairing::miller_loop({}, count).equal(fp12::one()));
   BOOST_CHECK(pairing::calculate({}, count).equal(fp12::one()));
   BOOST_CHECK_EQUAL(yields, 0u);
} FC_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()

