#pragma once

/// @file peer_auth.hpp
/// @brief Peer-to-peer authentication for net_plugin connections.
///
/// ## Protocol overview
///
/// After the TCP handshake and the initial `handshake_message` exchange, each
/// peer optionally authenticates by sending a `peer_auth_message{key, sig}`.
/// Whether authentication is required depends on the `allowed-connection`
/// configuration (see `possible_connections`).
///
/// The authentication exchange works as follows:
///
///   1. Both peers exchange `handshake_message`, which contains each node's
///      random `node_id` and the `chain_id`.
///
///   2. If `needs_auth()` is true, each peer computes an authentication digest:
///
///          digest = SHA-256(remote_node_id || my_node_id || chain_id)
///
///      The argument order is asymmetric — each side places the *remote* node
///      first.  This means A's digest differs from B's, preventing an attacker
///      from replaying A's signed message back to A as if it came from B.
///
///   3. Each peer signs the digest with its configured private key and sends
///      `peer_auth_message{public_key, signature}` to the other side.
///
///   4. The receiver recomputes the expected digest (from its own perspective),
///      recovers the signer from the signature, checks it matches the claimed
///      key, and then checks `is_key_authorized()` against the local policy.
///
/// ## Connection policies (`possible_connections`)
///
///   - `None`      — reject all peers (node is isolated).
///   - `Any`       — accept all peers without authentication.
///   - `Producers` — require auth; accept keys known to the producer plugin.
///   - `Specified` — require auth; accept keys listed in `allowed_peers` or
///                   present in `private_keys`.
///
///   Policies can be OR'd together (e.g. `Producers | Specified`).
///
/// ## Key authorization sources (checked in order)
///
///   1. `allowed_peers`       — explicit public key allow-list (`--peer-key`).
///   2. `private_keys`        — locally configured key pairs (`--peer-private-key`).
///   3. `is_producer_key_func`— callback into the producer plugin to check
///                              whether the key belongs to a registered producer.

#include <sysio/chain/types.hpp>
#include <fc/io/raw.hpp>
#include <fc/crypto/sha256.hpp>
#include <algorithm>
#include <functional>
#include <map>
#include <vector>

namespace sysio::peer_auth {
   using namespace chain;

   enum possible_connections : char {
      None = 0,
      Producers = 1 << 0,
      Specified = 1 << 1,
      Any = 1 << 2
   };

   /// Compute the authentication digest for peer-to-peer authentication.
   /// digest = SHA-256(remote_node_id || my_node_id || chain_id)
   /// Note: argument order matters — each peer puts the remote node first,
   /// producing asymmetric digests that prevent replay attacks.
   inline fc::sha256 compute_auth_digest(const fc::sha256&    remote_node_id,
                                         const fc::sha256&    my_node_id,
                                         const chain_id_type& chain_id) {
      fc::sha256::encoder enc;
      fc::raw::pack(enc, remote_node_id);
      fc::raw::pack(enc, my_node_id);
      fc::raw::pack(enc, chain_id);
      return enc.result();
   }

   /// Encapsulates peer authentication policy: which connections are allowed,
   /// which keys are trusted, and how to sign/verify authentication digests.
   struct peer_auth_config {
      possible_connections allowed_connections{None};
      std::vector<public_key_type> allowed_peers;
      std::map<public_key_type, private_key_type> private_keys;
      std::function<bool(const public_key_type&)> is_producer_key_func;

      /// Returns true when the current connection policy requires authentication.
      bool needs_auth() const {
         return (allowed_connections & (Producers | Specified)) != 0;
      }

      /// Check whether a given public key is authorized to connect under the
      /// current policy. Does not log — callers should log on failure.
      bool is_key_authorized(const public_key_type& key) const {
         if (allowed_connections == None)
            return false;
         if (allowed_connections == Any)
            return true;
         if (allowed_connections & (Producers | Specified)) {
            if (std::find(allowed_peers.begin(), allowed_peers.end(), key) != allowed_peers.end())
               return true;
            if (private_keys.count(key))
               return true;
            if (is_producer_key_func && is_producer_key_func(key))
               return true;
            return false;
         }
         return true;
      }

      /// Return the first configured private key's public key for use as
      /// authentication identity. Returns empty key if none configured.
      public_key_type get_authentication_key() const {
         if (!private_keys.empty())
            return private_keys.begin()->first;
         return {};
      }

      /// Sign a digest using the private key corresponding to `signer`.
      /// Returns a default (empty) signature if the key is not found.
      signature_type sign_compact(const public_key_type& signer, const fc::sha256& digest) const {
         auto it = private_keys.find(signer);
         if (it != private_keys.end())
            return it->second.sign(digest);
         return {};
      }
   };

} // namespace sysio::peer_auth
