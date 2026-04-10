#include <sysio/action.hpp>
#include <sysio/crypto.hpp>
#include <sysio/permission.hpp>

#include "sysio.msig.hpp"

namespace sysio {

transaction_header get_trx_header(const char* ptr, size_t sz);
bool trx_is_authorized(const std::vector<permission_level>& approvals, const std::vector<char>& packed_trx);

/// Returns true if `prop` was stored as a chunked proposal (its `packed_transaction` lives
/// in the `propchunks` table). Treats absent / zero `chunk_count` as not chunked.
static bool is_chunked(const multisig::proposal& prop) {
   return prop.chunk_count.has_value() && *prop.chunk_count > 0;
}

/// Reassembles the full serialized inner trx for `prop`. For non-chunked proposals this
/// just returns `prop.packed_transaction`; for chunked proposals it reads every row of
/// the `propchunks` table for `(self, proposer)` in `chunk_index` order and concatenates.
/// Asserts on missing chunks or size mismatch — both indicate corrupted state.
static std::vector<char> assemble_packed_trx(const multisig::proposal& prop, name self, name proposer) {
   if (!is_chunked(prop)) {
      return prop.packed_transaction;
   }
   const uint32_t n     = *prop.chunk_count;
   const uint32_t total = prop.total_size.has_value() ? *prop.total_size : 0;
   std::vector<char> out;
   out.reserve(total);
   multisig::propchunks chunktable(self, proposer.value);
   for (uint32_t i = 0; i < n; ++i) {
      const auto c = chunktable.get(multisig::propchunk_key{prop.proposal_name.value, i}, "missing proposal chunk");
      out.insert(out.end(), c.data.begin(), c.data.end());
   }
   check(out.size() == total, "chunk reassembly size mismatch");
   return out;
}

/// Returns the transaction_header for `prop` without reassembling the entire blob: pulls it
/// from `prop.packed_transaction` for non-chunked proposals, or from chunk 0 alone (the header
/// always lives within the first ~80 bytes, well under `proposal_chunk_size`) for chunked
/// proposals. Avoids the cost of reading every chunk just to check expiration / delay_sec.
static transaction_header read_trx_header(const multisig::proposal& prop, name self, name proposer) {
   if (!is_chunked(prop)) {
      return get_trx_header(prop.packed_transaction.data(), prop.packed_transaction.size());
   }
   multisig::propchunks chunktable(self, proposer.value);
   const auto c = chunktable.get(multisig::propchunk_key{prop.proposal_name.value, 0}, "missing proposal chunk 0");
   return get_trx_header(c.data.data(), c.data.size());
}

/// Erases all `propchunks` rows for a chunked proposal. No-op for non-chunked proposals.
/// Called from both `exec` and `cancel` so chunk rows never outlive their parent proposal.
static void erase_proposal_chunks(const multisig::proposal& prop, name self, name proposer) {
   if (!is_chunked(prop)) return;
   multisig::propchunks chunktable(self, proposer.value);
   const uint32_t n = *prop.chunk_count;
   for (uint32_t i = 0; i < n; ++i) {
      chunktable.erase(multisig::propchunk_key{prop.proposal_name.value, i});
   }
}

template<typename Function>
std::vector<permission_level> get_approvals_and_adjust_table(name self, name proposer, name proposal_name, Function&& table_op) {
   multisig::approvals approval_table( self, proposer.value );
   auto ak = multisig::approval_key{proposal_name.value};
   std::vector<permission_level> approvals_vector;
   multisig::invalidations invalidations_table( self );

   if ( approval_table.contains(ak) ) {
      auto app = approval_table.get(ak);
      approvals_vector.reserve( app.provided_approvals.size() );
      for ( const auto& permission : app.provided_approvals ) {
         auto ik = multisig::inval_key{permission.level.actor.value};
         if ( !invalidations_table.contains(ik) ) {
            approvals_vector.push_back(permission.level);
         } else {
            auto inv = invalidations_table.get(ik);
            if ( inv.last_invalidation_time < permission.time ) {
               approvals_vector.push_back(permission.level);
            }
         }
      }
      table_op( approval_table, ak );
   } else {
      multisig::old_approvals old_approval_table( self, proposer.value );
      auto oak = multisig::old_approval_key{proposal_name.value};
      const auto old_approvals_obj = old_approval_table.get( oak, "proposal not found" );
      for ( const auto& permission : old_approvals_obj.provided_approvals ) {
         auto ik = multisig::inval_key{permission.actor.value};
         if ( !invalidations_table.contains(ik) ) {
            approvals_vector.push_back( permission );
         }
      }
      table_op( old_approval_table, oak );
   }
   return approvals_vector;
}

void multisig::propose( name proposer,
                        name proposal_name,
                        std::vector<permission_level> requested,
                        ignore<transaction> trx )
{
   require_auth( proposer );
   auto& ds = get_datastream();

   const char* trx_pos = ds.pos();
   size_t size = ds.remaining();

   transaction_header trx_header;
   std::vector<action> context_free_actions;
   ds >> trx_header;
   check( trx_header.expiration >= sysio::time_point_sec(current_time_point()), "transaction expired" );
   ds >> context_free_actions;
   check( context_free_actions.empty(), "not allowed to `propose` a transaction with context-free actions" );

   proposals proptable( get_self(), proposer.value );
   auto pk = proposal_key{proposal_name.value};
   check( !proptable.contains(pk), "proposal with the same name exists" );

   auto packed_requested = pack(requested);
   auto res =  check_transaction_authorization(
         trx_pos, size,
         (const char*)0, 0,
         packed_requested.data(), packed_requested.size()
   );

   check( res > 0, "transaction authorization failed" );

   // Hash the inner trx once at propose time so `approve --proposal-hash` does not need to
   // reassemble chunks on every call. Same value regardless of chunked vs inline storage.
   const sysio::checksum256 trx_hash = sysio::sha256(trx_pos, static_cast<uint32_t>(size));

   if (size <= proposal_chunk_size) {
      // Inline path: small proposal stored as a single `proposal` row exactly as before.
      // External tooling that reads `packed_transaction` directly via `get_table_rows`
      // continues to work for this case.
      std::vector<char> pkd_trans;
      pkd_trans.resize(size);
      memcpy((char*)pkd_trans.data(), trx_pos, size);

      proptable.emplace( proposer, pk, proposal{
         .proposal_name      = proposal_name,
         .packed_transaction = std::move(pkd_trans),
         .earliest_exec_time = binary_extension< std::optional<time_point> >{},
         .chunk_count        = uint32_t{0},
         .total_size         = static_cast<uint32_t>(size),
         .trx_hash           = trx_hash,
      });
   } else {
      // Chunked path: split the inner trx across N rows of `propchunks`. The parent
      // `proposal` row carries an empty `packed_transaction` plus the chunk metadata
      // and the precomputed hash; clients use `getproposal` to retrieve the full blob.
      const uint32_t n_chunks = static_cast<uint32_t>((size + proposal_chunk_size - 1) / proposal_chunk_size);

      propchunks chunktable( get_self(), proposer.value );
      for (uint32_t i = 0; i < n_chunks; ++i) {
         const size_t off = static_cast<size_t>(i) * proposal_chunk_size;
         const size_t len = (size - off < proposal_chunk_size) ? (size - off) : proposal_chunk_size;
         std::vector<char> chunk_data;
         chunk_data.resize(len);
         memcpy((char*)chunk_data.data(), trx_pos + off, len);
         chunktable.emplace( proposer, propchunk_key{proposal_name.value, i}, propchunk{
            .proposal_name = proposal_name,
            .chunk_index   = i,
            .data          = std::move(chunk_data),
         });
      }

      proptable.emplace( proposer, pk, proposal{
         .proposal_name      = proposal_name,
         .packed_transaction = {},                       // empty: signals chunked storage
         .earliest_exec_time = binary_extension< std::optional<time_point> >{},
         .chunk_count        = n_chunks,
         .total_size         = static_cast<uint32_t>(size),
         .trx_hash           = trx_hash,
      });
   }

   approvals apptable( get_self(), proposer.value );
   std::vector<multisig::approval> req_approvals;
   req_approvals.reserve( requested.size() );
   for ( auto& level : requested ) {
      req_approvals.push_back( multisig::approval{ level, time_point{ microseconds{0} } } );
   }
   apptable.emplace( proposer, approval_key{proposal_name.value}, approvals_info{
      .version = 1,
      .proposal_name = proposal_name,
      .requested_approvals = std::move(req_approvals),
      .provided_approvals = {},
   });
}

void multisig::approve( name proposer, name proposal_name, permission_level level,
                        const sysio::binary_extension<sysio::checksum256>& proposal_hash )
{
   require_auth( level );

   proposals proptable( get_self(), proposer.value );
   auto pk = proposal_key{proposal_name.value};
   const auto prop = proptable.get( pk, "proposal not found" );

   if( proposal_hash ) {
      if (is_chunked(prop)) {
         // Chunked proposals have an empty `packed_transaction` field — can't re-hash from
         // it, so use the precomputed `trx_hash` stored at propose time. This avoids the
         // cost of reassembling the chunks on every approve call.
         check( prop.trx_hash.has_value() && *prop.trx_hash == *proposal_hash,
                "hash provided does not match stored proposal trx_hash" );
      } else {
         // Inline proposals: hash the inline blob directly. Same path the contract took
         // before chunked storage was introduced; `assert_sha256` is the chain intrinsic
         // and is the historical hash semantic external tooling depends on.
         assert_sha256( prop.packed_transaction.data(), prop.packed_transaction.size(), *proposal_hash );
      }
   }

   approvals apptable( get_self(), proposer.value );
   auto ak = approval_key{proposal_name.value};
   if ( apptable.contains(ak) ) {
      auto apps = apptable.get(ak);
      auto itr = std::find_if( apps.requested_approvals.begin(), apps.requested_approvals.end(), [&](const multisig::approval& a) { return a.level == level; } );
      check( itr != apps.requested_approvals.end(), "approval is not on the list of requested approvals" );

      apptable.modify( proposer, ak, [&]( auto& a ) {
         a.provided_approvals.push_back( multisig::approval{ level, current_time_point() } );
         a.requested_approvals.erase( std::find_if( a.requested_approvals.begin(), a.requested_approvals.end(), [&](const multisig::approval& ap) { return ap.level == level; } ) );
      });
   } else {
      old_approvals old_apptable( get_self(), proposer.value );
      auto oak = old_approval_key{proposal_name.value};
      auto apps = old_apptable.get( oak, "proposal not found" );

      auto itr = std::find( apps.requested_approvals.begin(), apps.requested_approvals.end(), level );
      check( itr != apps.requested_approvals.end(), "approval is not on the list of requested approvals" );

      old_apptable.modify( proposer, oak, [&]( auto& a ) {
         a.provided_approvals.push_back( level );
         a.requested_approvals.erase( std::find( a.requested_approvals.begin(), a.requested_approvals.end(), level ) );
      });
   }

   // Header parse only needs ~80 bytes — read from the inline blob or chunk 0 to avoid
   // reassembling the entire packed_transaction just to inspect delay_sec/expiration.
   transaction_header trx_header = read_trx_header(prop, get_self(), proposer);

   if( prop.earliest_exec_time.has_value() ) {
      if( !prop.earliest_exec_time->has_value() ) {
         auto table_op = [](auto&&, auto&&){};
         // The auth recheck needs the full blob. For inline proposals this is just
         // `prop.packed_transaction`; for chunked it reassembles. The cost is incurred at
         // most once per proposal — the first approve that pushes it over the threshold.
         const auto packed = assemble_packed_trx(prop, get_self(), proposer);
         if( trx_is_authorized(get_approvals_and_adjust_table(get_self(), proposer, proposal_name, table_op), packed) ) {
            proptable.modify( proposer, pk, [&]( auto& p ) {
               p.earliest_exec_time.emplace(time_point{ current_time_point() + sysio::seconds(trx_header.delay_sec.value)});
            });
         }
      }
   } else {
      check( trx_header.delay_sec.value == 0, "old proposals are not allowed to have non-zero `delay_sec`; cancel and retry" );
   }
}

void multisig::unapprove( name proposer, name proposal_name, permission_level level ) {
   require_auth( level );

   approvals apptable( get_self(), proposer.value );
   auto ak = approval_key{proposal_name.value};
   if ( apptable.contains(ak) ) {
      auto apps = apptable.get(ak);
      auto itr = std::find_if( apps.provided_approvals.begin(), apps.provided_approvals.end(), [&](const multisig::approval& a) { return a.level == level; } );
      check( itr != apps.provided_approvals.end(), "no approval previously granted" );
      apptable.modify( proposer, ak, [&]( auto& a ) {
         a.requested_approvals.push_back( multisig::approval{ level, current_time_point() } );
         a.provided_approvals.erase( std::find_if( a.provided_approvals.begin(), a.provided_approvals.end(), [&](const multisig::approval& ap) { return ap.level == level; } ) );
      });
   } else {
      old_approvals old_apptable( get_self(), proposer.value );
      auto oak = old_approval_key{proposal_name.value};
      auto apps = old_apptable.get( oak, "proposal not found" );
      auto itr = std::find( apps.provided_approvals.begin(), apps.provided_approvals.end(), level );
      check( itr != apps.provided_approvals.end(), "no approval previously granted" );
      old_apptable.modify( proposer, oak, [&]( auto& a ) {
         a.requested_approvals.push_back( level );
         a.provided_approvals.erase( std::find( a.provided_approvals.begin(), a.provided_approvals.end(), level ) );
      });
   }

   proposals proptable( get_self(), proposer.value );
   auto pk = proposal_key{proposal_name.value};
   const auto prop = proptable.get( pk, "proposal not found" );

   if( prop.earliest_exec_time.has_value() ) {
      if( prop.earliest_exec_time->has_value() ) {
         auto table_op = [](auto&&, auto&&){};
         // Reassemble for chunked proposals — same one-time cost pattern as approve.
         const auto packed = assemble_packed_trx(prop, get_self(), proposer);
         if( !trx_is_authorized(get_approvals_and_adjust_table(get_self(), proposer, proposal_name, table_op), packed) ) {
            proptable.modify( proposer, pk, [&]( auto& p ) {
               p.earliest_exec_time.emplace();
            });
         }
      }
   } else {
      transaction_header trx_header = read_trx_header(prop, get_self(), proposer);
      check( trx_header.delay_sec.value == 0, "old proposals are not allowed to have non-zero `delay_sec`; cancel and retry" );
   }
}

void multisig::cancel( name proposer, name proposal_name, name canceler ) {
   require_auth( canceler );

   proposals proptable( get_self(), proposer.value );
   auto pk = proposal_key{proposal_name.value};
   const auto prop = proptable.get( pk, "proposal not found" );

   if( canceler != proposer ) {
      // Header parse is chunk-aware: pulls from inline blob or chunk 0 as appropriate.
      check( read_trx_header(prop, get_self(), proposer).expiration < sysio::time_point_sec(current_time_point()),
             "cannot cancel until expiration" );
   }
   proptable.erase(pk);

   // Free chunk rows so they never outlive the parent proposal. No-op for inline proposals.
   erase_proposal_chunks(prop, get_self(), proposer);

   //remove from new table
   approvals apptable( get_self(), proposer.value );
   auto ak = approval_key{proposal_name.value};
   if ( apptable.contains(ak) ) {
      apptable.erase(ak);
   } else {
      old_approvals old_apptable( get_self(), proposer.value );
      auto oak = old_approval_key{proposal_name.value};
      check( old_apptable.contains(oak), "proposal not found" );
      old_apptable.erase(oak);
   }
}

void multisig::exec( name proposer, name proposal_name, name executer ) {
   require_auth( executer );

   proposals proptable( get_self(), proposer.value );
   auto pk = proposal_key{proposal_name.value};
   const auto prop = proptable.get( pk, "proposal not found" );

   // Reassemble the inner trx from chunks (or just take the inline blob) so we have one
   // contiguous buffer to feed into the deserializer. This is the only place in `exec`
   // that touches the chunked storage layout — everything below operates on `packed`.
   const auto packed = assemble_packed_trx(prop, get_self(), proposer);

   transaction_header trx_header;
   std::vector<action> context_free_actions;
   std::vector<action> actions;
   datastream<const char*> ds( packed.data(), packed.size() );
   ds >> trx_header;
   check( trx_header.expiration >= sysio::time_point_sec(current_time_point()), "transaction expired" );
   ds >> context_free_actions;
   check( context_free_actions.empty(), "not allowed to `exec` a transaction with context-free actions" );
   ds >> actions;

   auto table_op = [](auto&& table, auto&& key) { table.erase(key); };
   bool ok = trx_is_authorized(get_approvals_and_adjust_table(get_self(), proposer, proposal_name, table_op), packed);
   check( ok, "transaction authorization failed" );

   if ( prop.earliest_exec_time.has_value() && prop.earliest_exec_time->has_value() ) {
      check( **prop.earliest_exec_time <= current_time_point(), "too early to execute" );
   } else {
      check( trx_header.delay_sec.value == 0, "old proposals are not allowed to have non-zero `delay_sec`; cancel and retry" );
   }

   for (const auto& act : actions) {
      act.send();
   }

   proptable.erase(pk);
   // Free chunk rows after successful exec — same RAM-cleanup contract as cancel.
   erase_proposal_chunks(prop, get_self(), proposer);
}

void multisig::invalidate( name account ) {
   require_auth( account );
   invalidations inv_table( get_self() );
   auto ik = inval_key{account.value};
   inv_table.upsert( account, ik,
      invalidation{ .account = account, .last_invalidation_time = current_time_point() },
      [&](auto& i) { i.last_invalidation_time = current_time_point(); } );
}

multisig::proposal multisig::get_proposal( name proposer, name proposal_name ) {
   // Read-only action: invoked via /v1/chain/send_read_only_transaction. CDT auto-generates
   // `set_action_return_value(packed_result.data(), packed_result.size())` for non-void
   // actions, and the chain skips `max_action_return_value_size` in read-only context, so
   // we can return arbitrarily large reassembled blobs without bumping any chain limit.
   proposals proptable( get_self(), proposer.value );
   const auto prop = proptable.get( proposal_key{proposal_name.value}, "proposal not found" );

   proposal out = prop;
   // For chunked proposals, replace the empty `packed_transaction` field with the assembled
   // blob. For inline proposals this just copies the already-populated field.
   out.packed_transaction = assemble_packed_trx(prop, get_self(), proposer);
   // chunk_count / total_size / trx_hash stay populated so callers can verify what they got.
   return out;
}

transaction_header get_trx_header(const char* ptr, size_t sz) {
   datastream<const char*> ds = {ptr, sz};
   transaction_header trx_header;
   ds >> trx_header;
   return trx_header;
}

bool trx_is_authorized(const std::vector<permission_level>& approvals, const std::vector<char>& packed_trx) {
   auto packed_approvals = pack(approvals);
   return check_transaction_authorization(
         packed_trx.data(), packed_trx.size(),
         (const char*)0, 0,
         packed_approvals.data(), packed_approvals.size()
   );
}

} /// namespace sysio
