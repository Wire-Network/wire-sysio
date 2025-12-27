// unittests/test_signature_utils.hpp
#pragma once

#include <sysio/chain/transaction.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/elliptic_ed.hpp>   // for ed::public_key_shim
#include <fc/crypto/sha256.hpp>
#include <fc/io/raw.hpp>
#include <vector>
#include <cstdint>
#include <cstring>  // for std::memcpy

namespace test {

static constexpr uint16_t ED_EXTENSION_ID = 0x8000;

/// RFC 8032 Test Vector #1 public key (32 bytes)
inline fc::crypto::ed::public_key_shim make_rfc8032_pubkey() {
    std::array<unsigned char, crypto_sign_PUBLICKEYBYTES> arr;
    constexpr unsigned char bytes[crypto_sign_PUBLICKEYBYTES] = {
        // pubkey from RFC8032 vector
        0xd7,0x5a,0x98,0x01,  0x82,0xb1,0x0a,0xb7,
        0xd5,0x4b,0xfe,0xd3,  0xc9,0x64,0x07,0x3a,
        0x0e,0xe1,0x72,0xf3,  0xfa,0xa6,0x23,0x25,
        0xaf,0x02,0x1a,0x68,  0xf7,0x07,0x51,0x1a
    };
    std::memcpy(arr.data(), bytes, crypto_sign_PUBLICKEYBYTES);
    return fc::crypto::ed::public_key_shim{arr};
}

/// ED pubkey for extension‐mismatch tests
static const fc::crypto::ed::public_key_shim hardcoded_ed_pubkey = make_rfc8032_pubkey();

/// Build a signed_transaction that signs with each recoverable key.
/// If include_ed_ext==true *and* any signature is non-recoverable, it
/// would append that matching pubkey as an extension—but here you
/// only call it with recoverable keys.
inline sysio::chain::signed_transaction make_signed_trx(
    const std::vector<fc::crypto::private_key>& privs,
    const sysio::chain::chain_id_type&          chain_id,
    bool                                        include_ed_ext = false
) {
    using namespace sysio::chain;

    signed_transaction trx;
    trx.set_reference_block(block_id_type());
    auto tp = fc::time_point::now() + fc::seconds(3600);
    trx.expiration = fc::time_point_sec(tp);

    auto digest = trx.sig_digest(chain_id);

    for (auto& priv : privs) {
        // this will always be a recoverable signature (e.g. K1)
        auto sig = priv.sign(digest);
        trx.signatures.emplace_back(sig);

        // only if a non‑recoverable path ever happens:
        if (include_ed_ext && !sig.is_recoverable()) {
            auto pub = priv.get_public_key();
            trx.transaction_extensions.emplace_back(
                ED_EXTENSION_ID,
                fc::raw::pack(pub)
            );
        }
    }

    return trx;
}

} // namespace test
