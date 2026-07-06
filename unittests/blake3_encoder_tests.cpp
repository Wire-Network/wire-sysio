#include <sysio/chain/blake3_encoder.hpp>
#include <fc/crypto/blake3.hpp>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <vector>

using namespace sysio::chain;

namespace {

/// Build the official BLAKE3 test-vector input of the given length: byte i is
/// (i % 251). This mirrors the input generator used by the upstream BLAKE3
/// test_vectors.json, so the expected digests below are its published reference
/// values.
std::vector<char> blake3_vector_input(size_t len) {
   std::vector<char> v(len);
   for (size_t i = 0; i < len; ++i)
      v[i] = static_cast<char>(i % 251);
   return v;
}

/// A published BLAKE3-256 known-answer: the digest of blake3_vector_input(len).
struct blake3_kat {
   size_t      len;
   const char* hex; // first 32 bytes (BLAKE3-256) of the extended reference output, lowercase hex
};

// Taken verbatim from the upstream BLAKE3 test_vectors.json. These pin the
// digest bytes so that swapping the BLAKE3 provider (e.g. LLVM's bundled copy
// vs. the standalone library) or building without the sys-vm-oc runtime can
// never silently change the hash. blake3_encoder feeds snapshot identity that
// nodes compare across the network, so the bytes are consensus-relevant.
//
// The 102400-byte case is larger than blake3_encoder's 64 KiB internal buffer,
// exercising both the large-direct-update path and (in the streaming test) the
// fill-and-flush-at-buffer-boundary path.
constexpr blake3_kat k_blake3_vectors[] = {
   {0,      "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"},
   {1,      "2d3adedff11b61f14c886e35afa036736dcd87a74d27b5c1510225d0f592e213"},
   {3,      "e1be4d7a8ab5560aa4199eea339849ba8e293d55ca0a81006726d184519e647f"},
   {1024,   "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7"},
   {102400, "bc3e3d41a1146b069abffad3c0d44860cf664390afce4d9661f7902e7943e085"},
};

} // namespace

BOOST_AUTO_TEST_SUITE(blake3_encoder_tests)

// One-shot hashing must match the published BLAKE3 reference vectors exactly.
// This is the consensus guard for the BLAKE3-provider migration: regardless of
// which library backs blake3_encoder, the digest must be canonical BLAKE3.
BOOST_AUTO_TEST_CASE(known_answer_one_shot) {
   for (const auto& v : k_blake3_vectors) {
      const std::vector<char> in = blake3_vector_input(v.len);
      const fc::crypto::blake3 h = blake3_encoder::hash(in.data(), in.size());
      BOOST_CHECK_EQUAL(h.str(), v.hex);
   }
}

// Feeding the same input through the incremental write()/result() path in small,
// irregular chunks must yield the identical digest as the one-shot path. The
// 102400-byte input crosses blake3_encoder's 64 KiB buffer boundary several
// times, exercising the fill-then-flush logic.
BOOST_AUTO_TEST_CASE(streaming_matches_one_shot) {
   const std::vector<char> in = blake3_vector_input(102400);
   blake3_encoder enc;
   for (size_t off = 0; off < in.size(); ) {
      const size_t chunk = std::min<size_t>(7, in.size() - off); // odd stride to misalign with the buffer size
      enc.write(in.data() + off, chunk);
      off += chunk;
   }
   BOOST_CHECK_EQUAL(enc.result().str(), "bc3e3d41a1146b069abffad3c0d44860cf664390afce4d9661f7902e7943e085");
}

// reset() must return the encoder to its initial state so it can be reused to
// produce a fresh, independent digest.
BOOST_AUTO_TEST_CASE(reset_restores_initial_state) {
   blake3_encoder enc;
   const std::vector<char> in = blake3_vector_input(3);
   enc.write(in.data(), in.size());
   BOOST_CHECK_EQUAL(enc.result().str(), "e1be4d7a8ab5560aa4199eea339849ba8e293d55ca0a81006726d184519e647f");

   enc.reset();
   // No input after reset -> the empty-message digest.
   BOOST_CHECK_EQUAL(enc.result().str(), "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
}

BOOST_AUTO_TEST_SUITE_END()
