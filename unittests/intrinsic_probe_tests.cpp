// Boost.Test driver for the non-kv intrinsic_probe contract.
// Companion to unittests/kv_intrinsic_probe_tests.cpp (PR #308). Exercises
// the crypto, compiler_builtins, privileged, resource/auth/producer, and
// console/IO host intrinsics with inputs CDT wrappers would never emit but a
// malicious contract can: zero-length spans, wasm-boundary-crossing pointers,
// unaligned aligned_ptr targets, corrupt-but-well-formed signatures, and so on.
//
// Shared fixture mirrors PR #308's pattern: one validating_tester constructed
// once per process, both a non-privileged and a privileged account host the
// same probe WASM so the driver can select which account pushes each action.
// Privilege is required for get_resource_limits / set_resource_limits /
// get_blockchain_parameters_packed / set_blockchain_parameters_packed /
// preactivate_feature per their REGISTER_ALIGNED_HOST_FUNCTION(_, privileged_check)
// registration in runtimes/sys-vm.cpp.

#include <boost/test/unit_test.hpp>

#include <sysio/testing/tester.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/protocol_feature_manager.hpp>
#include <sysio/chain/protocol_feature_activation.hpp>

#include <fc/crypto/private_key.hpp>
#include <fc/crypto/public_key.hpp>
#include <fc/crypto/signature.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/io/raw.hpp>

#include <test_contracts.hpp>

#include <cstring>
#include <string>

using namespace sysio;
using namespace sysio::chain;
using namespace sysio::testing;

namespace {

constexpr auto probe_account      = "intprobe"_n;
constexpr auto probe_priv_account = "intprobe2"_n;

// Layout of the action-data payload the recover_key / assert_recover_key
// probes expect -- mirrors sig_hash_key_header in intrinsic_probe.cpp.
// Bytes:  [32] digest  [4] pk_len  [4] sig_len  [sig_len] sig  [pk_len] pub
constexpr std::size_t sig_hash_key_hdr_size = 32 + 4 + 4;

// Deterministic inputs for the recover_key test vector. WIF string is a
// well-known K1 test private key; the fixed message ensures sig + digest are
// reproducible run to run so the contract's recok probe memcmps against a
// pub value that we can regenerate here.
constexpr auto probe_priv_wif = "5KQwrPbwdL6PhXujxW37FSSQZ1JiwsST4cqQzDeyXtP79zkvFD3";
constexpr auto probe_msg_seed = "wire-intrinsic-probe-message";

// -----------------------------------------------------------------------------
// intrinsic_probe_shared_tester:
//   * setup_policy::preactivate_feature_and_new_bios so set_privileged() can
//     route through sysio.bios::setpriv, and so reserved_first_protocol_feature
//     remains unactivated for the preactok probe.
//   * Deploys the probe WASM to both accounts.
//   * Precomputes (feature_digest, recover_key_payload) once, exposes them to
//     individual test cases.
// -----------------------------------------------------------------------------
struct intrinsic_probe_shared_tester : validating_tester {
   digest_type unactivated_feature_digest;
   bytes       recover_key_payload;
   bytes       ed_recover_key_payload;
   bytes       ed_wrong_pub_payload;
   bytes       ed_wrong_digest_payload;
   bytes       oversized_wa_payload;

   intrinsic_probe_shared_tester()
      : validating_tester(flat_set<account_name>{}, nullptr,
                          setup_policy::preactivate_feature_and_new_bios) {
      create_accounts({ probe_account, probe_priv_account });
      produce_block();

      set_code(probe_account,      test_contracts::intrinsic_probe_wasm());
      set_abi (probe_account,      test_contracts::intrinsic_probe_abi().c_str());
      set_code(probe_priv_account, test_contracts::intrinsic_probe_wasm());
      set_abi (probe_priv_account, test_contracts::intrinsic_probe_abi().c_str());
      set_privileged(probe_priv_account);
      produce_block();

      const auto& pfm = control->get_protocol_feature_manager();
      auto d = pfm.get_builtin_digest(
         builtin_protocol_feature_t::reserved_first_protocol_feature );
      BOOST_REQUIRE_MESSAGE(d,
         "reserved_first_protocol_feature must be registered but unactivated "
         "in the probe fixture");
      unactivated_feature_digest = *d;

      build_recover_key_payload();
      build_ed_recover_key_payloads();
      build_oversized_wa_payload();
   }

   // Pack the (digest, sig, pub) triple into the sig_hash_key_header wire layout the
   // recover_key / assert_recover_key probe actions expect.
   static bytes pack_sig_hash_key(const fc::sha256& digest,
                                  const fc::crypto::signature& sig,
                                  const fc::crypto::public_key& pub) {
      const auto packed_sig = fc::raw::pack(sig);
      const auto packed_pub = fc::raw::pack(pub);

      bytes payload(sig_hash_key_hdr_size + packed_sig.size() + packed_pub.size());
      char* p = payload.data();
      std::memcpy(p, digest.data(), 32);                p += 32;
      const uint32_t pk_len  = static_cast<uint32_t>(packed_pub.size());
      const uint32_t sig_len = static_cast<uint32_t>(packed_sig.size());
      std::memcpy(p, &pk_len,  sizeof(pk_len));         p += sizeof(pk_len);
      std::memcpy(p, &sig_len, sizeof(sig_len));        p += sizeof(sig_len);
      std::memcpy(p, packed_sig.data(), sig_len);       p += sig_len;
      std::memcpy(p, packed_pub.data(), pk_len);
      return payload;
   }

   // Build (digest, sig, pub) triple from a deterministic seed so the probe
   // can be re-run and cross-checked against a source-embedded expectation.
   void build_recover_key_payload() {
      const auto priv = fc::crypto::private_key::from_string(probe_priv_wif);
      const auto digest = fc::sha256::hash(std::string{probe_msg_seed});
      recover_key_payload = pack_sig_hash_key(digest, priv.sign(digest), priv.get_public_key());
   }

   // ed25519 (digest, sig, pub) triples. There is no deterministic regenerate-from-seed path for
   // ed keys (the 64-byte libsodium secret key embeds its public half, so arbitrary seed bytes do
   // not form a valid keypair); fresh keypairs are generated per run and every assertion below is
   // self-consistent against them.
   //   * ed_recover_key_payload   -- golden triple: recover_key and assert_recover_key must BOTH
   //                                 accept it, pinning that the two intrinsics share one
   //                                 verification path (raw-vs-hex digest regression guard).
   //   * ed_wrong_pub_payload     -- sig by key A, pub of key B: recovery succeeds and returns A's
   //                                 embedded pub, so assert_recover_key must throw on the compare.
   //   * ed_wrong_digest_payload  -- valid triple but a different digest: ed verification fails,
   //                                 so recover_key returns -1 and assert_recover_key throws.
   void build_ed_recover_key_payloads() {
      using fc::crypto::private_key;
      const auto priv = private_key::generate(private_key::key_type::ed);
      const auto pub = priv.get_public_key();
      const auto digest = fc::sha256::hash(std::string{probe_msg_seed});
      const auto sig = priv.sign(digest);

      ed_recover_key_payload = pack_sig_hash_key(digest, sig, pub);

      const auto other_pub = private_key::generate(private_key::key_type::ed).get_public_key();
      ed_wrong_pub_payload = pack_sig_hash_key(digest, sig, other_pub);

      const auto wrong_digest = fc::sha256::hash(std::string{"wire-intrinsic-probe-ed-wrong-digest"});
      ed_wrong_digest_payload = pack_sig_hash_key(wrong_digest, sig, pub);
   }

   // Build a raw WA-variant signature blob whose auth_data + client_json variable-size components exceed the
   // subjective 16 KiB default. The bytes do not need to be a cryptographically valid sig -- fc::raw::unpack must
   // succeed so variable_size() is computed, after which the recover_key subjective-size guard fires before any
   // secp256k1 math runs. Layout:
   //   byte 0            : fc::raw variant tag = 2 (webauthn)
   //   bytes 1-65        : r1::compact_signature (65 zero bytes)
   //   <varint> + bytes  : auth_data  (WA_AUTH_DATA_BYTES bytes of zero)
   //   <varint> + bytes  : client_json (WA_CLIENT_JSON_BYTES bytes of zero)
   void build_oversized_wa_payload() {
      constexpr size_t wa_compact_sig_bytes = 65;
      constexpr size_t wa_variant_tag = 2;
      // 17 KiB total -- 1 KiB over the 16 KiB default configured_subjective_signature_length_limit.
      constexpr size_t wa_auth_data_bytes   = 8'700;
      constexpr size_t wa_client_json_bytes = 8'700;

      auto pack_varint = [](bytes& b, uint32_t v) {
         while (v > 0x7F) { b.push_back(static_cast<char>((v & 0x7F) | 0x80)); v >>= 7; }
         b.push_back(static_cast<char>(v));
      };

      bytes& b = oversized_wa_payload;
      b.clear();
      b.reserve(1 + wa_compact_sig_bytes + 5 + wa_auth_data_bytes + 5 + wa_client_json_bytes);
      b.push_back(static_cast<char>(wa_variant_tag));
      for (size_t i = 0; i < wa_compact_sig_bytes; ++i) b.push_back(0);
      pack_varint(b, wa_auth_data_bytes);
      for (size_t i = 0; i < wa_auth_data_bytes; ++i)   b.push_back(0);
      pack_varint(b, wa_client_json_bytes);
      for (size_t i = 0; i < wa_client_json_bytes; ++i) b.push_back(0);
   }

   bytes feature_digest_bytes() const {
      bytes b(32);
      std::memcpy(b.data(), unactivated_feature_digest.data(), 32);
      return b;
   }

   // ---- per-test-case entry points ----
   void run(name action_name) {
      push(probe_account, action_name, bytes{});
   }
   void run_priv(name action_name) {
      push(probe_priv_account, action_name, bytes{});
   }
   void run_with_data(name action_name, const bytes& d) {
      push(probe_account, action_name, d);
   }
   void run_priv_with_data(name action_name, const bytes& d) {
      push(probe_priv_account, action_name, d);
   }

private:
   void push(name acct, name action_name, bytes data) {
      signed_transaction trx;
      trx.actions.emplace_back(
         vector<permission_level>{{acct, config::active_name}},
         acct,
         action_name,
         std::move(data)
      );
      set_transaction_headers(trx);
      trx.sign(get_private_key(acct, "active"), control->get_chain_id());
      push_transaction(trx);
      produce_block();
   }
};

// Per-test-case fixture -- thin wrapper that reuses the shared tester. Same
// singleton pattern PR #308 uses so tester construction (bios load, account
// setup, contract deploy x2, privilege bump, feature-digest lookup,
// recover-key payload build) happens once, not per case.
struct intrinsic_probe_fixture {
   intrinsic_probe_shared_tester& t;
   intrinsic_probe_fixture() : t(shared_instance()) {}
   static intrinsic_probe_shared_tester& shared_instance() {
      static intrinsic_probe_shared_tester inst;
      return inst;
   }
};

} // namespace

BOOST_AUTO_TEST_SUITE(intrinsic_probe_tests)

// =============================================================================
// A. Hash intrinsics: sha256 / sha1 / sha512 / ripemd160
// =============================================================================

BOOST_FIXTURE_TEST_CASE(sha256_golden,    intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("sha2ok"_n));  }
BOOST_FIXTURE_TEST_CASE(sha256_empty,     intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("sha2em"_n));  }
BOOST_FIXTURE_TEST_CASE(sha256_big,       intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("sha2big"_n)); }
BOOST_FIXTURE_TEST_CASE(sha256_unaligned, intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("sha2ual"_n)); }

BOOST_FIXTURE_TEST_CASE(sha1_golden,    intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("sha1ok"_n));  }
BOOST_FIXTURE_TEST_CASE(sha1_empty,     intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("sha1em"_n));  }
BOOST_FIXTURE_TEST_CASE(sha1_big,       intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("sha1big"_n)); }
BOOST_FIXTURE_TEST_CASE(sha1_unaligned, intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("sha1ual"_n)); }

BOOST_FIXTURE_TEST_CASE(sha512_golden,    intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("sha5ok"_n));  }
BOOST_FIXTURE_TEST_CASE(sha512_empty,     intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("sha5em"_n));  }
BOOST_FIXTURE_TEST_CASE(sha512_big,       intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("sha5big"_n)); }
BOOST_FIXTURE_TEST_CASE(sha512_unaligned, intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("sha5ual"_n)); }

BOOST_FIXTURE_TEST_CASE(ripemd_golden,    intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("ripeok"_n));  }
BOOST_FIXTURE_TEST_CASE(ripemd_empty,     intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("ripeem"_n));  }
BOOST_FIXTURE_TEST_CASE(ripemd_big,       intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("ripebig"_n)); }
BOOST_FIXTURE_TEST_CASE(ripemd_unaligned, intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("ripeual"_n)); }

// =============================================================================
// B. assert_sha256 / assert_sha1 / assert_sha512 / assert_ripemd160
//
// Mismatch case throws crypto_api_exception via the host's SYS_ASSERT in
// assert_* impls (libraries/chain/webassembly/crypto.cpp).
// =============================================================================

BOOST_FIXTURE_TEST_CASE(assert_sha256_ok,        intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("asha2ok"_n)); }
BOOST_FIXTURE_TEST_CASE(assert_sha256_empty,     intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("asha2em"_n)); }
BOOST_FIXTURE_TEST_CASE(assert_sha256_unaligned, intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("asha2ua"_n)); }
BOOST_FIXTURE_TEST_CASE(assert_sha256_mismatch,  intrinsic_probe_fixture) {
   BOOST_CHECK_THROW(t.run("asha2ng"_n), crypto_api_exception);
}

BOOST_FIXTURE_TEST_CASE(assert_sha1_ok,        intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("asha1ok"_n)); }
BOOST_FIXTURE_TEST_CASE(assert_sha1_empty,     intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("asha1em"_n)); }
BOOST_FIXTURE_TEST_CASE(assert_sha1_unaligned, intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("asha1ua"_n)); }
BOOST_FIXTURE_TEST_CASE(assert_sha1_mismatch,  intrinsic_probe_fixture) {
   BOOST_CHECK_THROW(t.run("asha1ng"_n), crypto_api_exception);
}

BOOST_FIXTURE_TEST_CASE(assert_sha512_ok,        intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("asha5ok"_n)); }
BOOST_FIXTURE_TEST_CASE(assert_sha512_empty,     intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("asha5em"_n)); }
BOOST_FIXTURE_TEST_CASE(assert_sha512_unaligned, intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("asha5ua"_n)); }
BOOST_FIXTURE_TEST_CASE(assert_sha512_mismatch,  intrinsic_probe_fixture) {
   BOOST_CHECK_THROW(t.run("asha5ng"_n), crypto_api_exception);
}

BOOST_FIXTURE_TEST_CASE(assert_ripemd_ok,        intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("aripeok"_n)); }
BOOST_FIXTURE_TEST_CASE(assert_ripemd_empty,     intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("aripeem"_n)); }
BOOST_FIXTURE_TEST_CASE(assert_ripemd_unaligned, intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("aripeua"_n)); }
BOOST_FIXTURE_TEST_CASE(assert_ripemd_mismatch,  intrinsic_probe_fixture) {
   BOOST_CHECK_THROW(t.run("aripeng"_n), crypto_api_exception);
}

// =============================================================================
// C. recover_key / assert_recover_key
//
// Exception types reflect the three distinct failure paths: fc::raw::unpack
// (structural), public_key::recover (secp256k1 math), and SYS_ASSERT in the
// host impl (type match + pub compare). See the write-up in the contract
// section header for why fc::exception is intentionally used as the broad
// catch for paths where the host throws a generic fc::assert_exception
// without a typed sysio::chain wrapper. The exception-cleanup follow-on PR
// noted in the contract comments would tighten several of these to
// crypto_api_exception uniformly.
// =============================================================================

BOOST_FIXTURE_TEST_CASE(recover_key_golden, intrinsic_probe_fixture) {
   BOOST_CHECK_NO_THROW(t.run_with_data("recok"_n, t.recover_key_payload));
}

BOOST_FIXTURE_TEST_CASE(recover_key_small_pub, intrinsic_probe_fixture) {
   // Post-normalization (crypto.cpp:recover_key): small pub buffer returns the full required key size without writing
   // past the window or throwing. The probe's in-contract canary check verifies no buffer overrun; the driver just
   // asserts the host stays silent.
   BOOST_CHECK_NO_THROW(t.run_with_data("recsmpub"_n, t.recover_key_payload));
}

BOOST_FIXTURE_TEST_CASE(recover_key_unaligned_digest, intrinsic_probe_fixture) {
   BOOST_CHECK_NO_THROW(t.run_with_data("recuald"_n, t.recover_key_payload));
}

// Contract-observable failure modes now return -1 rather than throwing. The in-contract probe verifies
// `rc == -1`; the driver just asserts the host stays silent. Any host regression back to throwing surfaces
// as BOOST_CHECK_NO_THROW failure; a regression to "silently returns positive size for bad input" surfaces
// as a probe-side sysio_assert_message_exception.
BOOST_FIXTURE_TEST_CASE(recover_key_bad_variant, intrinsic_probe_fixture) {
   BOOST_CHECK_NO_THROW(t.run_with_data("recbadvar"_n, t.recover_key_payload));
}

BOOST_FIXTURE_TEST_CASE(recover_key_bad_recovery_byte, intrinsic_probe_fixture) {
   BOOST_CHECK_NO_THROW(t.run_with_data("recbadrec"_n, t.recover_key_payload));
}

BOOST_FIXTURE_TEST_CASE(recover_key_corrupt_rs, intrinsic_probe_fixture) {
   // A single-bit flip in the r component has two acceptable outcomes: the secp256k1 recovery succeeds and yields
   // a DIFFERENT pub (probe's in-contract memcmp passes), OR the math rejects the curve point and the probe sees
   // rc == -1. Both branches pass the probe; the bad outcome being protected against is "recovery silently
   // succeeds and returns the ORIGINAL pub", which is still a probe-side assert.
   BOOST_CHECK_NO_THROW(t.run_with_data("recbadrs"_n, t.recover_key_payload));
}

BOOST_FIXTURE_TEST_CASE(recover_key_short_sig, intrinsic_probe_fixture) {
   BOOST_CHECK_NO_THROW(t.run_with_data("recshort"_n, t.recover_key_payload));
}

BOOST_FIXTURE_TEST_CASE(recover_key_empty_sig, intrinsic_probe_fixture) {
   BOOST_CHECK_NO_THROW(t.run("recempsig"_n));
}

// Pins the size-query contract: zero-size pub buffer returns the full required key size so callers can allocate
// exactly, then re-call.
BOOST_FIXTURE_TEST_CASE(recover_key_size_query, intrinsic_probe_fixture) {
   BOOST_CHECK_NO_THROW(t.run_with_data("recqsize"_n, t.recover_key_payload));
}

// Pins the ONE throw recover_key still raises after the never-throw cleanup: subjective DoS guard on WebAuthn
// variable-size (auth_data + client_json) in speculative-block mode. See the DEFERRED note in
// libraries/chain/webassembly/crypto.cpp::recover_key for why this stays throwing rather than returning -1.
BOOST_FIXTURE_TEST_CASE(recover_key_wa_oversized_variable_size, intrinsic_probe_fixture) {
   BOOST_CHECK_THROW(
      t.run_with_data("recbigwa"_n, t.oversized_wa_payload),
      sig_variable_size_limit_exception);
}

BOOST_FIXTURE_TEST_CASE(assert_recover_key_ok, intrinsic_probe_fixture) {
   BOOST_CHECK_NO_THROW(t.run_with_data("arecok"_n, t.recover_key_payload));
}

BOOST_FIXTURE_TEST_CASE(assert_recover_key_mismatch, intrinsic_probe_fixture) {
   BOOST_CHECK_THROW(
      t.run_with_data("arecng"_n, t.recover_key_payload), crypto_api_exception);
}

BOOST_FIXTURE_TEST_CASE(assert_recover_key_bad_recovery, intrinsic_probe_fixture) {
   BOOST_CHECK_THROW(
      t.run_with_data("arecbadrec"_n, t.recover_key_payload), fc::exception);
}

BOOST_FIXTURE_TEST_CASE(assert_recover_key_pubtype, intrinsic_probe_fixture) {
   BOOST_CHECK_THROW(
      t.run_with_data("arecpubty"_n, t.recover_key_payload), crypto_api_exception);
}

BOOST_FIXTURE_TEST_CASE(assert_recover_key_empty_sig, intrinsic_probe_fixture) {
   BOOST_CHECK_THROW(t.run("arecempsig"_n), fc::exception);
}

// -----------------------------------------------------------------------------
// ed25519 agreement: recover_key and assert_recover_key must accept and reject
// the SAME (digest, sig, pub) triples. assert_recover_key previously verified
// ed signatures over the raw 32-byte digest while recover_key (and transaction
// authorization) verified over the hex-encoded digest, so every SDK-signed ed
// signature passed recover_key but failed assert_recover_key. Both now share
// the public_key::recover path; these cases pin that agreement.
// -----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(recover_key_ed_golden, intrinsic_probe_fixture) {
   BOOST_CHECK_NO_THROW(t.run_with_data("recok"_n, t.ed_recover_key_payload));
}

BOOST_FIXTURE_TEST_CASE(assert_recover_key_ed_golden, intrinsic_probe_fixture) {
   BOOST_CHECK_NO_THROW(t.run_with_data("arecok"_n, t.ed_recover_key_payload));
}

// Signature by key A, expected pub of key B: ed recovery succeeds (the signature verifies against
// its embedded key A) so the failure must come from the host's recovered-vs-expected compare.
BOOST_FIXTURE_TEST_CASE(assert_recover_key_ed_wrong_pub, intrinsic_probe_fixture) {
   BOOST_CHECK_THROW(
      t.run_with_data("arecok"_n, t.ed_wrong_pub_payload), crypto_api_exception);
}

// Different digest: ed verification fails inside signature_shim::recover. recover_key maps that
// to rc = -1 (the probe's rc==pk_len check then fires); assert_recover_key propagates the throw.
BOOST_FIXTURE_TEST_CASE(recover_key_ed_wrong_digest, intrinsic_probe_fixture) {
   BOOST_CHECK_THROW(
      t.run_with_data("recok"_n, t.ed_wrong_digest_payload), sysio_assert_message_exception);
}

BOOST_FIXTURE_TEST_CASE(assert_recover_key_ed_wrong_digest, intrinsic_probe_fixture) {
   BOOST_CHECK_THROW(
      t.run_with_data("arecok"_n, t.ed_wrong_digest_payload), fc::exception);
}

// =============================================================================
// D. preactivate_feature
//
// preactok uses reserved_first_protocol_feature's digest (registered but
// unactivated in our setup_policy::preactivate_feature_and_new_bios fixture).
// The action will *activate* the feature for the lifetime of the shared
// tester -- any test case that also needs the feature unactivated must run
// *before* this one. No such case currently exists.
// =============================================================================

BOOST_FIXTURE_TEST_CASE(preactivate_ok, intrinsic_probe_fixture) {
   BOOST_CHECK_NO_THROW(
      t.run_priv_with_data("preactok"_n, t.feature_digest_bytes()));
}

BOOST_FIXTURE_TEST_CASE(preactivate_nonpriv, intrinsic_probe_fixture) {
   BOOST_CHECK_THROW(t.run("preactnp"_n), unaccessible_api);
}

BOOST_FIXTURE_TEST_CASE(preactivate_bogus_digest, intrinsic_probe_fixture) {
   // All-zero digest -> controller rejects; in speculative-block context the
   // throw is subjective_block_production_exception, which is-a fc::exception.
   BOOST_CHECK_THROW(t.run_priv("preactbog"_n), fc::exception);
}

// E + F (compiler_builtins int128 + float128 host probes) removed: those host
// intrinsics were dropped, so probing the host ABI for them no longer applies.
// Contract-side librt coverage lives in api_tests.cpp::compiler_builtins_tests
// (drives test_compiler_builtins.cpp without sysio_wasm_import attributes).

// =============================================================================
// G. resource / auth / producer / blockchain-parameters (P2)
//
// get_resource_limits, get_blockchain_parameters_packed, set_resource_limits,
// set_proposed_producers[_ex], set_blockchain_parameters_packed are all
// REGISTER_ALIGNED_HOST_FUNCTION(..., privileged_check) -- their accept paths
// must run from the privileged account.
// get_active_producers and check_transaction_authorization are NOT priv-gated.
// =============================================================================

BOOST_FIXTURE_TEST_CASE(resource_limits_aligned,   intrinsic_probe_fixture) {
   BOOST_CHECK_NO_THROW(t.run_priv("reslimok"_n));
}
BOOST_FIXTURE_TEST_CASE(resource_limits_unaligned, intrinsic_probe_fixture) {
   BOOST_CHECK_NO_THROW(t.run_priv("reslimua"_n));
}
BOOST_FIXTURE_TEST_CASE(set_resource_limits_priv, intrinsic_probe_fixture) {
   BOOST_CHECK_NO_THROW(t.run_priv("setreslim"_n));
}
BOOST_FIXTURE_TEST_CASE(set_resource_limits_nonpriv, intrinsic_probe_fixture) {
   BOOST_CHECK_THROW(t.run("setresnp"_n), unaccessible_api);
}

BOOST_FIXTURE_TEST_CASE(active_producers_ok,        intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("actprdok"_n)); }
BOOST_FIXTURE_TEST_CASE(active_producers_size_only, intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("actprdsm"_n)); }

BOOST_FIXTURE_TEST_CASE(set_proposed_producers_nonpriv, intrinsic_probe_fixture) {
   BOOST_CHECK_THROW(t.run("sprodnp"_n), unaccessible_api);
}

BOOST_FIXTURE_TEST_CASE(bc_params_get_size, intrinsic_probe_fixture) {
   BOOST_CHECK_NO_THROW(t.run_priv("bcpgetsm"_n));
}
BOOST_FIXTURE_TEST_CASE(bc_params_get_ok, intrinsic_probe_fixture) {
   BOOST_CHECK_NO_THROW(t.run_priv("bcpgetok"_n));
}
BOOST_FIXTURE_TEST_CASE(bc_params_set_nonpriv, intrinsic_probe_fixture) {
   BOOST_CHECK_THROW(t.run("bcpsetnp"_n), unaccessible_api);
}

BOOST_FIXTURE_TEST_CASE(check_trx_auth_empty, intrinsic_probe_fixture) {
   BOOST_CHECK_THROW(t.run("chktrxem"_n), fc::exception);
}

// =============================================================================
// H. Console / IO (P3)
//
// sysio_assert / sysio_assert_message throw sysio_assert_message_exception
// on test == 0. send_inline with empty data fails during fc::raw::unpack of
// the action header (fc::exception).
// =============================================================================

BOOST_FIXTURE_TEST_CASE(prints_ok,        intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("printok"_n));  }
BOOST_FIXTURE_TEST_CASE(prints_l_empty,   intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("printlem"_n)); }
BOOST_FIXTURE_TEST_CASE(printhex_ok,      intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("phxok"_n));    }

BOOST_FIXTURE_TEST_CASE(sysio_assert_ok, intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("sasok"_n)); }
BOOST_FIXTURE_TEST_CASE(sysio_assert_fires, intrinsic_probe_fixture) {
   BOOST_CHECK_THROW(t.run("sasng"_n), sysio_assert_message_exception);
}

BOOST_FIXTURE_TEST_CASE(sysio_assert_message_ok, intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("samok"_n)); }
BOOST_FIXTURE_TEST_CASE(sysio_assert_message_fires, intrinsic_probe_fixture) {
   BOOST_CHECK_THROW(t.run("samng"_n), sysio_assert_message_exception);
}
BOOST_FIXTURE_TEST_CASE(sysio_assert_message_empty, intrinsic_probe_fixture) {
   BOOST_CHECK_THROW(t.run("samngem"_n), sysio_assert_message_exception);
}

BOOST_FIXTURE_TEST_CASE(read_action_data_ok,   intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("radok"_n)); }
BOOST_FIXTURE_TEST_CASE(read_action_data_zero, intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("radsm"_n)); }

BOOST_FIXTURE_TEST_CASE(get_action_ok,  intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("gacok"_n));  }
BOOST_FIXTURE_TEST_CASE(get_action_bad, intrinsic_probe_fixture) {
   // Invalid type (99) triggers SYS_ASSERT(act_ptr) in apply_context; the -1
   // sentinel path is only for valid-type / out-of-range-index.
   BOOST_CHECK_THROW(t.run("gacbad"_n), action_not_found_exception);
}

BOOST_FIXTURE_TEST_CASE(read_transaction_ok,   intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("rtxok"_n)); }
BOOST_FIXTURE_TEST_CASE(read_transaction_zero, intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("rtxsm"_n)); }

// get_context_free_data is registered context-free-only
// (REGISTER_ALIGNED_CF_ONLY_HOST_FUNCTION). Driven from a regular action its
// context_free_check precondition fires before the body and throws
// unaccessible_api, exactly like the privileged_check rejection probes. Pins
// that the CF-only gate -- which wraps the same aligned_span<char> adaptation
// the pointer->span cleanup reworks -- rejects the call before touching the
// span.
BOOST_FIXTURE_TEST_CASE(get_context_free_data_non_cf, intrinsic_probe_fixture) {
   BOOST_CHECK_THROW(t.run("gcfdcf"_n), unaccessible_api);
}

BOOST_FIXTURE_TEST_CASE(send_inline_empty, intrinsic_probe_fixture) {
   BOOST_CHECK_THROW(t.run("sinlem"_n), fc::exception);
}

BOOST_FIXTURE_TEST_CASE(set_action_return_value_ok,    intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("sarvok"_n)); }
BOOST_FIXTURE_TEST_CASE(set_action_return_value_empty, intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("sarvem"_n)); }

// =============================================================================
// I. get_permission_lower_bound (P2)
//
// Wire-specific intrinsic (libraries/chain/webassembly/permission.cpp) backing CDT's sysio::get_permission --
// the only host path a contract has to read another account's authority on-chain (sysio.roa::active_key_matches,
// sysio.uwrit). Each probe queries get_self()'s owner/active permissions (created with a single K1 key at
// threshold 1) and self-verifies the record via in-contract check(); the driver asserts the host stays silent,
// so any layout / size-query / sentinel / lower-bound-semantics regression surfaces as a
// sysio_assert_message_exception here. Not privileged-gated, so all run from the ordinary probe account.
// =============================================================================

BOOST_FIXTURE_TEST_CASE(get_permission_lower_bound_active_golden, intrinsic_probe_fixture) {
   BOOST_CHECK_NO_THROW(t.run("glpbact"_n));
}
BOOST_FIXTURE_TEST_CASE(get_permission_lower_bound_owner_root_parent, intrinsic_probe_fixture) {
   BOOST_CHECK_NO_THROW(t.run("glpbown"_n));
}
BOOST_FIXTURE_TEST_CASE(get_permission_lower_bound_size_query, intrinsic_probe_fixture) {
   BOOST_CHECK_NO_THROW(t.run("glpbsz"_n));
}
BOOST_FIXTURE_TEST_CASE(get_permission_lower_bound_next_not_exact, intrinsic_probe_fixture) {
   BOOST_CHECK_NO_THROW(t.run("glpbnx"_n));
}
BOOST_FIXTURE_TEST_CASE(get_permission_lower_bound_not_found, intrinsic_probe_fixture) {
   BOOST_CHECK_NO_THROW(t.run("glpbnf"_n));
}

BOOST_AUTO_TEST_SUITE_END()
