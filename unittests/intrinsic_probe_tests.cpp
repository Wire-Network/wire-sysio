// Boost.Test driver for the non-kv intrinsic_probe contract.
// Companion to unittests/kv_intrinsic_probe_tests.cpp (PR #308). Exercises
// the crypto, compiler_builtins, privileged, resource/auth/producer, and
// console/IO host intrinsics with inputs CDT wrappers would never emit but a
// malicious contract can: zero-length spans, wasm-boundary-crossing pointers,
// unaligned legacy_ptr targets, corrupt-but-well-formed signatures, and so on.
//
// Shared fixture mirrors PR #308's pattern: one validating_tester constructed
// once per process, both a non-privileged and a privileged account host the
// same probe WASM so the driver can select which account pushes each action.
// Privilege is required for get_resource_limits / set_resource_limits /
// get_blockchain_parameters_packed / set_blockchain_parameters_packed /
// preactivate_feature per their REGISTER_LEGACY_HOST_FUNCTION(_, privileged_check)
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
   }

   // Build (digest, sig, pub) triple from a deterministic seed so the probe
   // can be re-run and cross-checked against a source-embedded expectation.
   void build_recover_key_payload() {
      const auto priv = fc::crypto::private_key::from_string(probe_priv_wif);
      const auto pub = priv.get_public_key();
      const auto digest = fc::sha256::hash(std::string{probe_msg_seed});
      const auto sig = priv.sign(digest);

      const auto packed_sig = fc::raw::pack(sig);
      const auto packed_pub = fc::raw::pack(pub);

      recover_key_payload.resize(
         sig_hash_key_hdr_size + packed_sig.size() + packed_pub.size());
      char* p = recover_key_payload.data();
      std::memcpy(p, digest.data(), 32);                p += 32;
      const uint32_t pk_len  = static_cast<uint32_t>(packed_pub.size());
      const uint32_t sig_len = static_cast<uint32_t>(packed_sig.size());
      std::memcpy(p, &pk_len,  sizeof(pk_len));         p += sizeof(pk_len);
      std::memcpy(p, &sig_len, sizeof(sig_len));        p += sizeof(sig_len);
      std::memcpy(p, packed_sig.data(), sig_len);       p += sig_len;
      std::memcpy(p, packed_pub.data(), pk_len);
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
   // K1 fixed-size key -> fc::datastream::write FC_ASSERTs when dest < 34.
   BOOST_CHECK_THROW(
      t.run_with_data("recsmpub"_n, t.recover_key_payload), fc::exception);
}

BOOST_FIXTURE_TEST_CASE(recover_key_unaligned_digest, intrinsic_probe_fixture) {
   BOOST_CHECK_NO_THROW(t.run_with_data("recuald"_n, t.recover_key_payload));
}

BOOST_FIXTURE_TEST_CASE(recover_key_bad_variant, intrinsic_probe_fixture) {
   BOOST_CHECK_THROW(
      t.run_with_data("recbadvar"_n, t.recover_key_payload), fc::exception);
}

BOOST_FIXTURE_TEST_CASE(recover_key_bad_recovery_byte, intrinsic_probe_fixture) {
   BOOST_CHECK_THROW(
      t.run_with_data("recbadrec"_n, t.recover_key_payload), fc::exception);
}

BOOST_FIXTURE_TEST_CASE(recover_key_corrupt_rs, intrinsic_probe_fixture) {
   // A single-bit flip in the r component has two acceptable outcomes: the
   // secp256k1 recovery succeeds and yields a DIFFERENT pub (probe's
   // in-contract memcmp passes), OR the math fails (host throws
   // fc::exception). The bad outcome the probe is protecting against is
   // "recovery silently succeeds and returns the ORIGINAL pub", which never
   // happens with a real bit flip -- both branches are a correct pin.
   try {
      t.run_with_data("recbadrs"_n, t.recover_key_payload);
   } catch (const fc::exception&) {
      // Math failure path -- acceptable.
   }
}

BOOST_FIXTURE_TEST_CASE(recover_key_short_sig, intrinsic_probe_fixture) {
   BOOST_CHECK_THROW(
      t.run_with_data("recshort"_n, t.recover_key_payload), fc::exception);
}

BOOST_FIXTURE_TEST_CASE(recover_key_empty_sig, intrinsic_probe_fixture) {
   BOOST_CHECK_THROW(t.run("recempsig"_n), fc::exception);
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

// =============================================================================
// E. compiler_builtins: 128-bit integer ops
//
// Each intrinsic is covered by (golden, unaligned out-ptr, edge value) where
// relevant. Division-by-zero throws arithmetic_exception per the host's
// SYS_ASSERT in compiler_builtins.cpp::__divti3 / __udivti3.
// =============================================================================

BOOST_FIXTURE_TEST_CASE(int128_mul_ok,        intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("mulok"_n));   }
BOOST_FIXTURE_TEST_CASE(int128_mul_unaligned, intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("mulua"_n));   }
BOOST_FIXTURE_TEST_CASE(int128_mul_carry,     intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("muledge"_n)); }

BOOST_FIXTURE_TEST_CASE(int128_div_ok,        intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("divok"_n)); }
BOOST_FIXTURE_TEST_CASE(int128_div_unaligned, intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("divua"_n)); }
BOOST_FIXTURE_TEST_CASE(int128_div_zero,      intrinsic_probe_fixture) {
   BOOST_CHECK_THROW(t.run("divzero"_n), arithmetic_exception);
}

BOOST_FIXTURE_TEST_CASE(int128_udiv_ok,        intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("udivok"_n)); }
BOOST_FIXTURE_TEST_CASE(int128_udiv_unaligned, intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("udivua"_n)); }
BOOST_FIXTURE_TEST_CASE(int128_udiv_zero,      intrinsic_probe_fixture) {
   BOOST_CHECK_THROW(t.run("udivzero"_n), arithmetic_exception);
}

BOOST_FIXTURE_TEST_CASE(int128_ashl_ok,             intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("ashlok"_n));   }
BOOST_FIXTURE_TEST_CASE(int128_ashl_unaligned,      intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("ashlua"_n));   }
BOOST_FIXTURE_TEST_CASE(int128_ashl_shift_overflow, intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("ashlover"_n)); }

// =============================================================================
// F. compiler_builtins: float128 (quad precision) ops
// =============================================================================

BOOST_FIXTURE_TEST_CASE(f128_add_ok,        intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("addfok"_n)); }
BOOST_FIXTURE_TEST_CASE(f128_add_unaligned, intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("addfua"_n)); }

BOOST_FIXTURE_TEST_CASE(f128_mul_ok,        intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("mulfok"_n));  }
BOOST_FIXTURE_TEST_CASE(f128_mul_unaligned, intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("mulfua"_n));  }
BOOST_FIXTURE_TEST_CASE(f128_mul_nan,       intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("mulfnan"_n)); }

BOOST_FIXTURE_TEST_CASE(f128_div_ok,        intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("divfok"_n));   }
BOOST_FIXTURE_TEST_CASE(f128_div_unaligned, intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("divfua"_n));   }
BOOST_FIXTURE_TEST_CASE(f128_div_zero,      intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("divfzero"_n)); }

BOOST_FIXTURE_TEST_CASE(f128_fix_overflow, intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("fixovfl"_n)); }

// =============================================================================
// G. resource / auth / producer / blockchain-parameters (P2)
//
// get_resource_limits, get_blockchain_parameters_packed, set_resource_limits,
// set_proposed_producers[_ex], set_blockchain_parameters_packed are all
// REGISTER_LEGACY_HOST_FUNCTION(..., privileged_check) -- their accept paths
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

BOOST_FIXTURE_TEST_CASE(send_inline_empty, intrinsic_probe_fixture) {
   BOOST_CHECK_THROW(t.run("sinlem"_n), fc::exception);
}

BOOST_FIXTURE_TEST_CASE(set_action_return_value_ok,    intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("sarvok"_n)); }
BOOST_FIXTURE_TEST_CASE(set_action_return_value_empty, intrinsic_probe_fixture) { BOOST_CHECK_NO_THROW(t.run("sarvem"_n)); }

BOOST_AUTO_TEST_SUITE_END()
