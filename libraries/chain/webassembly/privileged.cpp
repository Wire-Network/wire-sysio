#include <sysio/chain/account_object.hpp>
#include <sysio/chain/webassembly/interface.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/protocol_state_object.hpp>
#include <sysio/chain/transaction_context.hpp>
#include <sysio/chain/resource_limits.hpp>
#include <sysio/chain/apply_context.hpp>
#include <sysio/chain/finalizer_policy.hpp>
#include <sysio/chain/finalizer_authority.hpp>

#include <fc/io/datastream.hpp>
#include <fc/io/json.hpp>

#include <vector>
#include <set>

namespace sysio { namespace chain { namespace webassembly {

   int interface::is_feature_active( int64_t feature_name ) const { return false; }

   void interface::activate_feature( int64_t feature_name ) const {
      SYS_ASSERT( false, unsupported_feature, "Unsupported Hardfork Detected" );
   }

   void interface::preactivate_feature( aligned_ptr<const digest_type> feature_digest ) {
      SYS_ASSERT(!context.trx_context.is_read_only(), wasm_execution_error, "preactivate_feature not allowed in a readonly transaction");
      context.control.preactivate_feature( *feature_digest, context.trx_context.is_transient() );
   }

   void interface::set_resource_limits( account_name account, int64_t ram_bytes, int64_t net_weight, int64_t cpu_weight ) {
      SYS_ASSERT(!context.trx_context.is_read_only(), wasm_execution_error, "set_resource_limits not allowed in a readonly transaction");
      SYS_ASSERT(ram_bytes >= -1, wasm_execution_error, "invalid value for ram resource limit expected [-1,INT64_MAX]");
      SYS_ASSERT(net_weight >= -1, wasm_execution_error, "invalid value for net resource weight expected [-1,INT64_MAX]");
      SYS_ASSERT(cpu_weight >= -1, wasm_execution_error, "invalid value for cpu resource weight expected [-1,INT64_MAX]");
      if( context.control.get_mutable_resource_limits_manager().set_account_limits(account, ram_bytes, net_weight, cpu_weight, context.trx_context.is_transient()) ) {
         context.trx_context.validate_ram_usage.insert( account );
      }
   }

   void interface::get_resource_limits( account_name account, aligned_ptr<int64_t> ram_bytes, aligned_ptr<int64_t> net_weight, aligned_ptr<int64_t> cpu_weight ) const {
      context.control.get_resource_limits_manager().get_account_limits( account, *ram_bytes, *net_weight, *cpu_weight);
      (void)aligned_ptr<int64_t>(std::move(ram_bytes));
      (void)aligned_ptr<int64_t>(std::move(net_weight));
      (void)aligned_ptr<int64_t>(std::move(cpu_weight));
   }

   int64_t set_proposed_producers_common( apply_context& context, vector<producer_authority>&& producers ) {
      // Structural validation (bounds, uniqueness, per-authority key/threshold sanity)
      // lives on proposer_policy::validate() so the snapshot-load path and this
      // intrinsic enforce identical invariants. Move the producers into a policy
      // shell for validation and re-use the shell's vector for the subsequent
      // per-producer checks and the final downstream call — avoids a copy.
      proposer_policy candidate;
      candidate.proposer_schedule.producers = std::move(producers);
      try {
         candidate.validate();
      } catch (const producer_schedule_exception& e) {
         SYS_THROW(wasm_execution_error, "set_proposed_producers: {}", e.top_message());
      }

      // Remaining checks that don't belong in proposer_policy::validate():
      //   - account existence (requires apply_context)
      //   - K1/R1 key type enforcement (uses unactivated_key_type to convey
      //     that non-K1/R1 keys need a protocol feature to be activated — a
      //     distinct category from structural validation errors)
      //   - key.valid() semantics
      using key_type = fc::crypto::public_key::key_type;
      for (const auto& p : candidate.proposer_schedule.producers) {
         SYS_ASSERT(context.is_account(p.producer_name), wasm_execution_error,
                    "producer schedule includes a nonexisting account");
         std::visit([&p](const auto& a) {
            for (const auto& kw : a.keys) {
               SYS_ASSERT(kw.key.contains_type(key_type::k1, key_type::r1), unactivated_key_type,
                          "Unactivated key type used in proposed producer schedule");
               SYS_ASSERT(kw.key.valid(), wasm_execution_error, "producer schedule includes an invalid key");
            }
         }, p.authority);
      }

      return context.control.set_proposed_producers( context.trx_context,
                                                     std::move(candidate.proposer_schedule.producers) );
   }

   uint32_t interface::get_wasm_parameters_packed( span<char> packed_parameters, uint32_t max_version ) const {
      auto& gpo = context.control.get_global_properties();
      auto& params = gpo.wasm_configuration;
      uint32_t version = std::min( max_version, uint32_t(0) );

      auto s = fc::raw::pack_size( version ) + fc::raw::pack_size( params );
      if ( packed_parameters.size() == 0 )
         return s;

      if ( s <= packed_parameters.size() ) {
         fc::datastream<char*> ds( packed_parameters.data(), s );
         fc::raw::pack(ds, version);
         fc::raw::pack(ds, params);
      }
      return s;
   }
   void interface::set_wasm_parameters_packed( span<const char> packed_parameters ) {
      SYS_ASSERT(!context.trx_context.is_read_only(), wasm_execution_error, "set_wasm_parameters_packed not allowed in a readonly transaction");
      fc::datastream<const char*> ds( packed_parameters.data(), packed_parameters.size() );
      uint32_t version;
      chain::wasm_config cfg;
      fc::raw::unpack(ds, version);
      SYS_ASSERT(version == 0, wasm_config_unknown_version, "set_wasm_parameters_packed: Unknown version: {}", version);
      fc::raw::unpack(ds, cfg);
      cfg.validate();
      context.db.modify( context.control.get_global_properties(),
         [&]( auto& gprops ) {
            gprops.wasm_configuration = cfg;
         }
      );
   }
   int64_t interface::set_proposed_producers( span<const char> packed_producer_schedule) {
      SYS_ASSERT(!context.trx_context.is_read_only(), wasm_execution_error, "set_proposed_producers not allowed in a readonly transaction");
      fc::datastream<const char*> ds( packed_producer_schedule.data(), packed_producer_schedule.size() );
      std::vector<producer_authority> producers;
      std::vector<legacy::producer_key> old_version;
      fc::raw::unpack(ds, old_version);

      /*
       * Up-convert the producers
       */
      for ( const auto& p : old_version ) {
         producers.emplace_back( producer_authority{ p.producer_name, block_signing_authority_v0{ 1, {{p.block_signing_key, 1}} } } );
      }

      return set_proposed_producers_common( context, std::move(producers) );
   }

   int64_t interface::set_proposed_producers_ex( uint64_t packed_producer_format, span<const char> packed_producer_schedule) {
      SYS_ASSERT(!context.trx_context.is_read_only(), wasm_execution_error, "set_proposed_producers_ex not allowed in a readonly transaction");
      if (packed_producer_format == 0) {
         return set_proposed_producers(std::move(packed_producer_schedule));
      } else if (packed_producer_format == 1) {
         fc::datastream<const char*> ds( packed_producer_schedule.data(), packed_producer_schedule.size() );
         vector<producer_authority> producers;

         fc::raw::unpack(ds, producers);
         return set_proposed_producers_common( context, std::move(producers) );
      } else {
         SYS_THROW(wasm_execution_error, "Producer schedule is in an unknown format!");
      }
   }

   // format for packed_finalizer_policy
   struct finalizer_authority {
      std::string              description;
      uint64_t                 weight = 0; // weight that this finalizer's vote has for meeting fthreshold
      std::vector<uint8_t>     public_key; // Affine little endian non-montgomery g1, cdt/abi_serializer has issues with std::array, size 96
   };
   struct finalizer_policy {
      uint64_t                          threshold = 0;
      std::vector<finalizer_authority>  finalizers;
   };

   void interface::set_finalizers(uint64_t packed_finalizer_format, span<const char> packed_finalizer_policy) {
      SYS_ASSERT(!context.trx_context.is_read_only(), wasm_execution_error,
                 "set_finalizers not allowed in a readonly transaction");
      if (packed_finalizer_format != 0) {
         SYS_THROW(wasm_execution_error, "Finalizer policy is in an unknown format!");
      }

      fc::datastream<const char*> ds( packed_finalizer_policy.data(), packed_finalizer_policy.size() );
      finalizer_policy abi_finpol;
      fc::raw::unpack(ds, abi_finpol);

      // Transform the ABI-provided payload into the chain-level finalizer_policy.
      // The per-key length check stays here because it is a wire-format guard on the
      // ABI input shape, not a property of the stored policy (the chain-level
      // bls_public_key type enforces size=96 at construction).
      chain::finalizer_policy finpol;
      finpol.threshold = abi_finpol.threshold;
      finpol.finalizers.reserve(abi_finpol.finalizers.size());
      for (auto& f: abi_finpol.finalizers) {
         SYS_ASSERT(f.public_key.size() == 96, wasm_execution_error, "Invalid bls public key length");
         fc::crypto::bls::public_key pk(std::span<const uint8_t,96>(f.public_key.data(), 96));
         finpol.finalizers.push_back(chain::finalizer_authority{.description = std::move(f.description),
                                                                .weight = f.weight,
                                                                .public_key{pk}});
      }

      // Structural validation is factored into finalizer_policy::validate() so the
      // snapshot-load path and this intrinsic enforce identical invariants.
      try {
         finpol.validate();
      } catch (const invalid_finalizer_policy_exception& e) {
         SYS_THROW(wasm_execution_error, "set_finalizers: {}", e.top_message());
      }

      context.trx_context.set_proposed_finalizers( std::move(finpol) );
   }

   uint32_t interface::get_blockchain_parameters_packed( span<char> packed_blockchain_parameters ) const {
      auto& gpo = context.control.get_global_properties();

      const chain::chain_config_v0& cfg = gpo.configuration;
      auto s = fc::raw::pack_size( cfg );
      if( packed_blockchain_parameters.size() == 0 ) return s;

      if ( s <= packed_blockchain_parameters.size() ) {
         fc::datastream<char*> ds( packed_blockchain_parameters.data(), s );
         fc::raw::pack(ds, cfg);
         return s;
      }
      return 0;
   }

   void interface::set_blockchain_parameters_packed( span<const char> packed_blockchain_parameters ) {
      SYS_ASSERT(!context.trx_context.is_read_only(), wasm_execution_error, "set_blockchain_parameters_packed not allowed in a readonly transaction");
      fc::datastream<const char*> ds( packed_blockchain_parameters.data(), packed_blockchain_parameters.size() );
      chain::chain_config_v0 cfg;
      fc::raw::unpack(ds, cfg);
      cfg.validate();
      context.db.modify( context.control.get_global_properties(),
         [&]( auto& gprops ) {
              gprops.configuration = cfg;
      });
   }
   
   uint32_t interface::get_parameters_packed( span<const char> packed_parameter_ids, span<char> packed_parameters) const{
      fc::datastream<const char*> ds_ids( packed_parameter_ids.data(), packed_parameter_ids.size() );

      chain::chain_config cfg = context.control.get_global_properties().configuration;
      std::vector<fc::unsigned_int> ids;
      fc::raw::unpack(ds_ids, ids);
      const config_range config_range(cfg, std::move(ids), {context.control});
      
      auto size = fc::raw::pack_size( config_range );
      if( packed_parameters.size() == 0 ) return size;

      SYS_ASSERT(size <= packed_parameters.size(),
                 chain::config_parse_error,
                 "get_parameters_packed: buffer size is smaller than {}", size);
      
      fc::datastream<char*> ds( packed_parameters.data(), size );
      fc::raw::pack( ds, config_range );
      return size;
   }

   void interface::set_parameters_packed( span<const char> packed_parameters ){
      SYS_ASSERT(!context.trx_context.is_read_only(), wasm_execution_error, "set_parameters_packed not allowed in a readonly transaction");
      fc::datastream<const char*> ds( packed_parameters.data(), packed_parameters.size() );

      chain::chain_config cfg = context.control.get_global_properties().configuration;
      config_range config_range(cfg, {context.control});

      fc::raw::unpack(ds, config_range);
      
      config_range.config.validate();
      context.db.modify( context.control.get_global_properties(),
         [&]( auto& gprops ) {
              gprops.configuration = config_range.config;
      });
   }

   bool interface::is_privileged( account_name n ) const {
      const auto* account_metadata = context.db.find<account_metadata_object, by_name>( n );
      return account_metadata != nullptr && account_metadata->is_privileged();
   }

   void interface::set_privileged( account_name n, bool is_priv ) {
      SYS_ASSERT(!context.trx_context.is_read_only(), wasm_execution_error, "set_privileged not allowed in a readonly transaction");
      const auto* account_metadata = context.db.find<account_metadata_object, by_name>( n );
      if (account_metadata == nullptr && !is_priv)
         return; // already not priv
      SYS_ASSERT(account_metadata != nullptr, wasm_execution_error, "setcode must be called before setpriv for account {}", n);
      context.db.modify( *account_metadata, [&]( auto& ma ){
         ma.set_privileged( is_priv );
      });
   }
}}} // ns sysio::chain::webassembly

FC_REFLECT(sysio::chain::webassembly::finalizer_authority, (description)(weight)(public_key));
FC_REFLECT(sysio::chain::webassembly::finalizer_policy, (threshold)(finalizers));
