// unittests/test_signature_utils.hpp
#pragma once

#include <sysio/chain/transaction.hpp>
#include <fc/crypto/private_key.hpp>
#include <fc/crypto/sha256.hpp>
#include <fc/io/raw.hpp>
#include <vector>
#include <cstdint>

namespace test {

/// Build a signed_transaction signed with each key.
/// All signature types (K1, ED, etc.) now go through recover(),
/// so no extensions are needed.
inline sysio::chain::signed_transaction make_signed_trx(
    const std::vector<fc::crypto::private_key>& privs,
    const sysio::chain::chain_id_type&          chain_id
) {
    using namespace sysio::chain;

    signed_transaction trx;
    trx.set_reference_block(block_id_type());
    auto tp = fc::time_point::now() + fc::seconds(3600);
    trx.expiration = fc::time_point_sec(tp);

    auto digest = trx.sig_digest(chain_id);

    for (auto& priv : privs) {
        trx.signatures.emplace_back(priv.sign(digest));
    }

    return trx;
}

} // namespace test
