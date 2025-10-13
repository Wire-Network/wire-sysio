#include <sysio/chain/protocol_feature_manager.hpp>
#include <sysio/chain/protocol_state_object.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/deep_mind.hpp>

#include <fc/scoped_exit.hpp>
#include <fc/io/json.hpp>

#include <algorithm>
#include <boost/assign/list_of.hpp>

namespace sysio { namespace chain {

   const std::unordered_map<builtin_protocol_feature_t, builtin_protocol_feature_spec, enum_hash<builtin_protocol_feature_t>>
   builtin_protocol_feature_codenames =
      boost::assign::map_list_of<builtin_protocol_feature_t, builtin_protocol_feature_spec>
         (  builtin_protocol_feature_t::preactivate_feature, builtin_protocol_feature_spec{
            "PREACTIVATE_FEATURE",
            fc::variant("64fe7df32e9b86be2b296b3f81dfd527f84e82b98e363bc97e40bc7a83733310").as<digest_type>(),
            // SHA256 hash of the raw message below within the comment delimiters (do not modify message below).
/*
Builtin protocol feature: PREACTIVATE_FEATURE

Adds privileged intrinsic to enable a contract to pre-activate a protocol feature specified by its digest.
Pre-activated protocol features must be activated in the next block.
*/
            {},
            {time_point{}, false, true} // enabled without preactivation and ready to go at any time
         } )
         (  builtin_protocol_feature_t::reserved_first_protocol_feature, builtin_protocol_feature_spec{
            "RESERVED_FIRST_PROTOCOL_FEATURE",
            fc::variant("7fed1e357f6973b66ea0e582ca0c1a69b3ce08e7e5780af010935007ac89e4e3").as<digest_type>(),
            // SHA256 hash of the raw message below within the comment delimiters (do not modify message below).
            // SHA256 begins with "Builtin ..." after return and includes trailing return.
/*
Builtin protocol feature: RESERVED_FIRST_PROTOCOL_FEATURE

Example protocol feature. No functionality is triggered by this protocol feature.
*/
            {},
            {time_point{}, false, true} // enabled without preactivation and ready to go at any time
         } )
         (  builtin_protocol_feature_t::reserved_second_protocol_feature, builtin_protocol_feature_spec{
            "RESERVED_SECOND_PROTOCOL_FEATURE",
            fc::variant("0c27fcedb5c663edaf95ade037820deb8d04a1a0b828edba1de58c06f380b3d5").as<digest_type>(),
            // SHA256 hash of the raw message below within the comment delimiters (do not modify message below).
            // SHA256 begins with "Builtin ..." after return and includes trailing return.
/*
Builtin protocol feature: RESERVED_SECOND_PROTOCOL_FEATURE

Example protocol feature. No functionality is triggered by this protocol feature.
*/
            {}
         } )
   ;


   const char* builtin_protocol_feature_codename( builtin_protocol_feature_t codename ) {
      auto itr = builtin_protocol_feature_codenames.find( codename );
      SYS_ASSERT( itr != builtin_protocol_feature_codenames.end(), protocol_feature_validation_exception,
                  "Unsupported builtin_protocol_feature_t passed to builtin_protocol_feature_codename: ${codename}",
                  ("codename", static_cast<uint32_t>(codename)) );

      return itr->second.codename;
   }

   protocol_feature_base::protocol_feature_base( protocol_feature_t feature_type,
                                                 const digest_type& description_digest,
                                                 flat_set<digest_type>&& dependencies,
                                                 const protocol_feature_subjective_restrictions& restrictions )
   :description_digest( description_digest )
   ,dependencies( std::move(dependencies) )
   ,subjective_restrictions( restrictions )
   ,_type( feature_type )
   {
      switch( feature_type ) {
         case protocol_feature_t::builtin:
            protocol_feature_type = builtin_protocol_feature::feature_type_string;
         break;
         default:
         {
            SYS_THROW( protocol_feature_validation_exception,
                       "Unsupported protocol_feature_t passed to constructor: ${type}",
                       ("type", static_cast<uint32_t>(feature_type)) );
         }
         break;
      }
   }

   void protocol_feature_base::reflector_init() {
      static_assert( fc::raw::has_feature_reflector_init_on_unpacked_reflected_types,
                     "protocol_feature_activation expects FC to support reflector_init" );

      if( protocol_feature_type == builtin_protocol_feature::feature_type_string ) {
         _type = protocol_feature_t::builtin;
      } else {
         SYS_THROW( protocol_feature_validation_exception,
                    "Unsupported protocol feature type: ${type}", ("type", protocol_feature_type) );
      }
   }

   const char* builtin_protocol_feature::feature_type_string = "builtin";

   builtin_protocol_feature::builtin_protocol_feature( builtin_protocol_feature_t codename,
                                                       const digest_type& description_digest,
                                                       flat_set<digest_type>&& dependencies,
                                                       const protocol_feature_subjective_restrictions& restrictions )
   :protocol_feature_base( protocol_feature_t::builtin, description_digest, std::move(dependencies), restrictions )
   ,_codename(codename)
   {
      auto itr = builtin_protocol_feature_codenames.find( codename );
      SYS_ASSERT( itr != builtin_protocol_feature_codenames.end(), protocol_feature_validation_exception,
                  "Unsupported builtin_protocol_feature_t passed to constructor: ${codename}",
                  ("codename", static_cast<uint32_t>(codename)) );

      builtin_feature_codename = itr->second.codename;
   }

   void builtin_protocol_feature::reflector_init() {
      protocol_feature_base::reflector_init();

      for( const auto& p : builtin_protocol_feature_codenames ) {
         if( builtin_feature_codename.compare( p.second.codename ) == 0 ) {
            _codename = p.first;
            return;
         }
      }

      SYS_THROW( protocol_feature_validation_exception,
                 "Unsupported builtin protocol feature codename: ${codename}",
                 ("codename", builtin_feature_codename) );
   }


   digest_type builtin_protocol_feature::digest()const {
      digest_type::encoder enc;
      fc::raw::pack( enc, _type );
      fc::raw::pack( enc, description_digest  );
      fc::raw::pack( enc, dependencies );
      fc::raw::pack( enc, _codename );

      return enc.result();
   }

   fc::variant protocol_feature::to_variant( bool include_subjective_restrictions,
                                             fc::mutable_variant_object* additional_fields )const
   {
      SYS_ASSERT( builtin_feature, protocol_feature_exception, "not a builtin protocol feature" );

      fc::mutable_variant_object mvo;

      mvo( "feature_digest", feature_digest );

      if( additional_fields ) {
         for( const auto& e : *additional_fields ) {
            if( e.key().compare( "feature_digest" ) != 0 )
               mvo( e.key(), e.value() );
         }
      }

      if( include_subjective_restrictions ) {
         fc::mutable_variant_object subjective_restrictions;

         subjective_restrictions( "enabled", enabled );
         subjective_restrictions( "preactivation_required", preactivation_required );
         subjective_restrictions( "earliest_allowed_activation_time", earliest_allowed_activation_time );

         mvo( "subjective_restrictions", std::move( subjective_restrictions ) );
      }

      mvo( "description_digest", description_digest );
      mvo( "dependencies", dependencies );
      mvo( "protocol_feature_type", builtin_protocol_feature::feature_type_string );

      fc::variants specification;
      auto add_to_specification = [&specification]( const char* key_name, auto&& value ) {
         fc::mutable_variant_object obj;
         obj( "name", key_name );
         obj( "value", std::forward<decltype(value)>( value ) );
         specification.emplace_back( std::move(obj) );
      };


      add_to_specification( "builtin_feature_codename", builtin_protocol_feature_codename( *builtin_feature ) );

      mvo( "specification", std::move( specification ) );

      return fc::variant( std::move(mvo) );
   }

   protocol_feature_set::protocol_feature_set()
   {
      _recognized_builtin_protocol_features.reserve( builtin_protocol_feature_codenames.size() );
   }


   protocol_feature_set::recognized_t
   protocol_feature_set::is_recognized( const digest_type& feature_digest, time_point now )const {
      auto itr = _recognized_protocol_features.find( feature_digest );

      if( itr == _recognized_protocol_features.end() )
         return recognized_t::unrecognized;

      if( !itr->enabled )
         return recognized_t::disabled;

      if( itr->earliest_allowed_activation_time > now )
         return recognized_t::too_early;

      return recognized_t::ready;
   }

   std::optional<digest_type> protocol_feature_set::get_builtin_digest( builtin_protocol_feature_t feature_codename )const {
      uint32_t indx = static_cast<uint32_t>( feature_codename );

      if( indx >= _recognized_builtin_protocol_features.size() )
         return {};

      if( _recognized_builtin_protocol_features[indx] == _recognized_protocol_features.end() )
         return {};

      return _recognized_builtin_protocol_features[indx]->feature_digest;
   }

   const protocol_feature& protocol_feature_set::get_protocol_feature( const digest_type& feature_digest )const {
      auto itr = _recognized_protocol_features.find( feature_digest );

      SYS_ASSERT( itr != _recognized_protocol_features.end(), protocol_feature_exception,
                  "unrecognized protocol feature with digest: ${digest}",
                  ("digest", feature_digest)
      );

      return *itr;
   }

   bool protocol_feature_set::validate_dependencies(
                                    const digest_type& feature_digest,
                                    const std::function<bool(const digest_type&)>& validator
   )const {
      auto itr = _recognized_protocol_features.find( feature_digest );

      if( itr == _recognized_protocol_features.end() ) return false;

      for( const auto& d : itr->dependencies ) {
         if( !validator(d) ) return false;
      }

      return true;
   }

   builtin_protocol_feature
   protocol_feature_set::make_default_builtin_protocol_feature(
      builtin_protocol_feature_t codename,
      const std::function<digest_type(builtin_protocol_feature_t dependency)>& handle_dependency
   ) {
      auto itr = builtin_protocol_feature_codenames.find( codename );

      SYS_ASSERT( itr != builtin_protocol_feature_codenames.end(), protocol_feature_validation_exception,
                  "Unsupported builtin_protocol_feature_t: ${codename}",
                  ("codename", static_cast<uint32_t>(codename)) );

      flat_set<digest_type> dependencies;
      dependencies.reserve( itr->second.builtin_dependencies.size() );

      for( const auto& d : itr->second.builtin_dependencies ) {
         dependencies.insert( handle_dependency( d ) );
      }

      return {itr->first, itr->second.description_digest, std::move(dependencies), itr->second.subjective_restrictions};
   }

   const protocol_feature& protocol_feature_set::add_feature( const builtin_protocol_feature& f ) {
      auto builtin_itr = builtin_protocol_feature_codenames.find( f._codename );
      SYS_ASSERT( builtin_itr != builtin_protocol_feature_codenames.end(), protocol_feature_validation_exception,
                  "Builtin protocol feature has unsupported builtin_protocol_feature_t: ${codename}",
                  ("codename", static_cast<uint32_t>( f._codename )) );

      uint32_t indx = static_cast<uint32_t>( f._codename );

      if( indx < _recognized_builtin_protocol_features.size() ) {
         SYS_ASSERT( _recognized_builtin_protocol_features[indx] == _recognized_protocol_features.end(),
                     protocol_feature_exception,
                     "builtin protocol feature with codename '${codename}' already added",
                     ("codename", f.builtin_feature_codename) );
      }

      auto feature_digest = f.digest();

      const auto& expected_builtin_dependencies = builtin_itr->second.builtin_dependencies;
      flat_set<builtin_protocol_feature_t> satisfied_builtin_dependencies;
      satisfied_builtin_dependencies.reserve( expected_builtin_dependencies.size() );

      for( const auto& d : f.dependencies ) {
         auto itr = _recognized_protocol_features.find( d );
         SYS_ASSERT( itr != _recognized_protocol_features.end(), protocol_feature_exception,
            "builtin protocol feature with codename '${codename}' and digest of ${digest} has a dependency on a protocol feature with digest ${dependency_digest} that is not recognized",
            ("codename", f.builtin_feature_codename)
            ("digest",  feature_digest)
            ("dependency_digest", d )
         );

         if( itr->builtin_feature
             && expected_builtin_dependencies.find( *itr->builtin_feature )
                  != expected_builtin_dependencies.end() )
         {
            satisfied_builtin_dependencies.insert( *itr->builtin_feature );
         }
      }

      if( expected_builtin_dependencies.size() > satisfied_builtin_dependencies.size() ) {
         flat_set<builtin_protocol_feature_t> missing_builtins;
         missing_builtins.reserve( expected_builtin_dependencies.size() - satisfied_builtin_dependencies.size() );
         std::set_difference( expected_builtin_dependencies.begin(), expected_builtin_dependencies.end(),
                              satisfied_builtin_dependencies.begin(), satisfied_builtin_dependencies.end(),
                              end_inserter( missing_builtins )
         );

         vector<string> missing_builtins_with_names;
         missing_builtins_with_names.reserve( missing_builtins.size() );
         for( const auto& builtin_codename : missing_builtins ) {
            auto itr = builtin_protocol_feature_codenames.find( builtin_codename );
            SYS_ASSERT( itr != builtin_protocol_feature_codenames.end(),
                        protocol_feature_exception,
                        "Unexpected error"
            );
            missing_builtins_with_names.emplace_back( itr->second.codename );
         }

         SYS_THROW(  protocol_feature_validation_exception,
                     "Not all the builtin dependencies of the builtin protocol feature with codename '${codename}' and digest of ${digest} were satisfied.",
                     ("missing_dependencies", missing_builtins_with_names)
         );
      }

      auto res = _recognized_protocol_features.insert( protocol_feature{
         feature_digest,
         f.description_digest,
         f.dependencies,
         f.subjective_restrictions.earliest_allowed_activation_time,
         f.subjective_restrictions.preactivation_required,
         f.subjective_restrictions.enabled,
         f._codename
      } );

      SYS_ASSERT( res.second, protocol_feature_exception,
                  "builtin protocol feature with codename '${codename}' has a digest of ${digest} but another protocol feature with the same digest has already been added",
                  ("codename", f.builtin_feature_codename)("digest", feature_digest) );

      if( indx >= _recognized_builtin_protocol_features.size() ) {
         for( auto i =_recognized_builtin_protocol_features.size(); i <= indx; ++i ) {
            _recognized_builtin_protocol_features.push_back( _recognized_protocol_features.end() );
         }
      }

      _recognized_builtin_protocol_features[indx] = res.first;
      return *res.first;
   }



   protocol_feature_manager::protocol_feature_manager(
      protocol_feature_set&& pfs,
      std::function<deep_mind_handler*(bool is_trx_transient)> get_deep_mind_logger
   ):_protocol_feature_set( std::move(pfs) ), _get_deep_mind_logger(get_deep_mind_logger)
   {
      _builtin_protocol_features.resize( _protocol_feature_set._recognized_builtin_protocol_features.size() );
   }

   void protocol_feature_manager::init( chainbase::database& db ) {
      SYS_ASSERT( !is_initialized(), protocol_feature_exception, "cannot initialize protocol_feature_manager twice" );


      auto reset_initialized = fc::make_scoped_exit( [this]() { _initialized = false; } );
      _initialized = true;

      for( const auto& f : db.get<protocol_state_object>().activated_protocol_features ) {
         activate_feature( f.feature_digest, f.activation_block_num );
      }

      reset_initialized.cancel();
   }

   const protocol_feature* protocol_feature_manager::const_iterator::get_pointer()const {
      //SYS_ASSERT( _pfm, protocol_feature_iterator_exception, "cannot dereference singular iterator" );
      //SYS_ASSERT( _index != end_index, protocol_feature_iterator_exception, "cannot dereference end iterator" );
      return &*(_pfm->_activated_protocol_features[_index].iterator_to_protocol_feature);
   }

   uint32_t protocol_feature_manager::const_iterator::activation_ordinal()const {
      SYS_ASSERT( _pfm,
                   protocol_feature_iterator_exception,
                  "called activation_ordinal() on singular iterator"
      );
      SYS_ASSERT( _index != end_index,
                   protocol_feature_iterator_exception,
                  "called activation_ordinal() on end iterator"
      );

      return _index;
   }

   uint32_t protocol_feature_manager::const_iterator::activation_block_num()const {
      SYS_ASSERT( _pfm,
                   protocol_feature_iterator_exception,
                  "called activation_block_num() on singular iterator"
      );
      SYS_ASSERT( _index != end_index,
                   protocol_feature_iterator_exception,
                  "called activation_block_num() on end iterator"
      );

      return _pfm->_activated_protocol_features[_index].activation_block_num;
   }

   protocol_feature_manager::const_iterator& protocol_feature_manager::const_iterator::operator++() {
      SYS_ASSERT( _pfm, protocol_feature_iterator_exception, "cannot increment singular iterator" );
      SYS_ASSERT( _index != end_index, protocol_feature_iterator_exception, "cannot increment end iterator" );

      ++_index;
      if( _index >= _pfm->_activated_protocol_features.size() ) {
         _index = end_index;
      }

      return *this;
   }

   protocol_feature_manager::const_iterator& protocol_feature_manager::const_iterator::operator--() {
      SYS_ASSERT( _pfm, protocol_feature_iterator_exception, "cannot decrement singular iterator" );
      if( _index == end_index ) {
         SYS_ASSERT( _pfm->_activated_protocol_features.size() > 0,
                     protocol_feature_iterator_exception,
                     "cannot decrement end iterator when no protocol features have been activated"
         );
         _index = _pfm->_activated_protocol_features.size() - 1;
      } else {
         SYS_ASSERT( _index > 0,
                     protocol_feature_iterator_exception,
                     "cannot decrement iterator at the beginning of protocol feature activation list" )
         ;
         --_index;
      }
      return *this;
   }

   protocol_feature_manager::const_iterator protocol_feature_manager::cbegin()const {
      if( _activated_protocol_features.size() == 0 ) {
         return cend();
      } else {
         return const_iterator( this, 0 );
      }
   }

   protocol_feature_manager::const_iterator
   protocol_feature_manager::at_activation_ordinal( uint32_t activation_ordinal )const {
      if( activation_ordinal >= _activated_protocol_features.size() ) {
         return cend();
      }

      return const_iterator{this, static_cast<std::size_t>(activation_ordinal)};
   }

   protocol_feature_manager::const_iterator
   protocol_feature_manager::lower_bound( uint32_t block_num )const {
      const auto begin = _activated_protocol_features.cbegin();
      const auto end   = _activated_protocol_features.cend();
      auto itr = std::lower_bound( begin, end, block_num, []( const protocol_feature_entry& lhs, uint32_t rhs ) {
         return lhs.activation_block_num < rhs;
      } );

      if( itr == end ) {
         return cend();
      }

      return const_iterator{this, static_cast<std::size_t>(itr - begin)};
   }

   protocol_feature_manager::const_iterator
   protocol_feature_manager::upper_bound( uint32_t block_num )const {
      const auto begin = _activated_protocol_features.cbegin();
      const auto end   = _activated_protocol_features.cend();
      auto itr = std::upper_bound( begin, end, block_num, []( uint32_t lhs, const protocol_feature_entry& rhs ) {
         return lhs < rhs.activation_block_num;
      } );

      if( itr == end ) {
         return cend();
      }

      return const_iterator{this, static_cast<std::size_t>(itr - begin)};
   }

   bool protocol_feature_manager::is_builtin_activated( builtin_protocol_feature_t feature_codename,
                                                        uint32_t current_block_num )const
   {
      uint32_t indx = static_cast<uint32_t>( feature_codename );

      if( indx >= _builtin_protocol_features.size() ) return false;

      return (_builtin_protocol_features[indx].activation_block_num <= current_block_num);
   }

   void protocol_feature_manager::activate_feature( const digest_type& feature_digest,
                                                    uint32_t current_block_num )
   {
      SYS_ASSERT( is_initialized(), protocol_feature_exception, "protocol_feature_manager is not yet initialized" );

      auto itr = _protocol_feature_set.find( feature_digest );

      SYS_ASSERT( itr != _protocol_feature_set.end(), protocol_feature_exception,
                  "unrecognized protocol feature digest: ${digest}", ("digest", feature_digest) );

      if( _activated_protocol_features.size() > 0 ) {
         const auto& last = _activated_protocol_features.back();
         SYS_ASSERT( last.activation_block_num <= current_block_num,
                     protocol_feature_exception,
                     "last protocol feature activation block num is ${last_activation_block_num} yet "
                     "attempting to activate protocol feature with a current block num of ${current_block_num}"
                     "protocol features is ${last_activation_block_num}",
                     ("current_block_num", current_block_num)
                     ("last_activation_block_num", last.activation_block_num)
         );
      }

      SYS_ASSERT( itr->builtin_feature,
                  protocol_feature_exception,
                  "invariant failure: encountered non-builtin protocol feature which is not yet supported"
      );

      uint32_t indx = static_cast<uint32_t>( *itr->builtin_feature );

      SYS_ASSERT( indx < _builtin_protocol_features.size(), protocol_feature_exception,
                  "invariant failure while trying to activate feature with digest '${digest}': "
                  "unsupported builtin_protocol_feature_t ${codename}",
                  ("digest", feature_digest)
                  ("codename", indx)
      );

      SYS_ASSERT( _builtin_protocol_features[indx].activation_block_num == builtin_protocol_feature_entry::not_active,
                  protocol_feature_exception,
                  "cannot activate already activated builtin feature with digest: ${digest}",
                  ("digest", feature_digest)
      );

      // activate_feature is called by init. no transaction specific logging is possible
      if (auto dm_logger = _get_deep_mind_logger(false)) {
         dm_logger->on_activate_feature(*itr);
      }

      _activated_protocol_features.push_back( protocol_feature_entry{itr, current_block_num} );
      _builtin_protocol_features[indx].previous = _head_of_builtin_activation_list;
      _builtin_protocol_features[indx].activation_block_num = current_block_num;
      _head_of_builtin_activation_list = indx;
   }

   void protocol_feature_manager::popped_blocks_to( uint32_t block_num ) {
      SYS_ASSERT( is_initialized(), protocol_feature_exception, "protocol_feature_manager is not yet initialized" );

      while( _head_of_builtin_activation_list != builtin_protocol_feature_entry::no_previous ) {
         auto& e = _builtin_protocol_features[_head_of_builtin_activation_list];
         if( e.activation_block_num <= block_num ) break;

         _head_of_builtin_activation_list = e.previous;
         e.previous = builtin_protocol_feature_entry::no_previous;
         e.activation_block_num = builtin_protocol_feature_entry::not_active;
      }

      while( _activated_protocol_features.size() > 0
              && block_num < _activated_protocol_features.back().activation_block_num )
      {
         _activated_protocol_features.pop_back();
      }
   }

   std::optional<builtin_protocol_feature> read_builtin_protocol_feature( const std::filesystem::path& p  ) {
      try {
         return fc::json::from_file<builtin_protocol_feature>( p );
      } catch( const fc::exception& e ) {
         wlog( "problem encountered while reading '${path}':\n${details}",
               ("path", p)("details",e.to_detail_string()) );
      } catch( ... ) {
         dlog( "unknown problem encountered while reading '${path}'",
               ("path", p) );
      }
      return {};
   }

   protocol_feature_set initialize_protocol_features( const std::filesystem::path& p, bool populate_missing_builtins ) {
      using std::filesystem::directory_iterator;

      protocol_feature_set pfs;

      bool directory_exists = true;

      if( std::filesystem::exists( p ) ) {
         SYS_ASSERT( std::filesystem::is_directory( p ), plugin_exception,
                     "Path to protocol-features is not a directory: ${path}",
                     ("path", p)
         );
      } else {
         if( populate_missing_builtins )
            std::filesystem::create_directories( p );
         else
            directory_exists = false;
      }

      auto log_recognized_protocol_feature = []( const builtin_protocol_feature& f, const digest_type& feature_digest ) {
         if( f.subjective_restrictions.enabled ) {
            if( f.subjective_restrictions.preactivation_required ) {
               if( f.subjective_restrictions.earliest_allowed_activation_time == time_point{} ) {
                  ilog( "Support for builtin protocol feature '${codename}' (with digest of '${digest}') is enabled with preactivation required",
                        ("codename", builtin_protocol_feature_codename(f.get_codename()))
                              ("digest", feature_digest)
                  );
               } else {
                  ilog( "Support for builtin protocol feature '${codename}' (with digest of '${digest}') is enabled with preactivation required and with an earliest allowed activation time of ${earliest_time}",
                        ("codename", builtin_protocol_feature_codename(f.get_codename()))
                              ("digest", feature_digest)
                              ("earliest_time", f.subjective_restrictions.earliest_allowed_activation_time)
                  );
               }
            } else {
               if( f.subjective_restrictions.earliest_allowed_activation_time == time_point{} ) {
                  ilog( "Support for builtin protocol feature '${codename}' (with digest of '${digest}') is enabled without activation restrictions",
                        ("codename", builtin_protocol_feature_codename(f.get_codename()))
                              ("digest", feature_digest)
                  );
               } else {
                  ilog( "Support for builtin protocol feature '${codename}' (with digest of '${digest}') is enabled without preactivation required but with an earliest allowed activation time of ${earliest_time}",
                        ("codename", builtin_protocol_feature_codename(f.get_codename()))
                              ("digest", feature_digest)
                              ("earliest_time", f.subjective_restrictions.earliest_allowed_activation_time)
                  );
               }
            }
         } else {
            ilog( "Recognized builtin protocol feature '${codename}' (with digest of '${digest}') but support for it is not enabled",
                  ("codename", builtin_protocol_feature_codename(f.get_codename()))
                        ("digest", feature_digest)
            );
         }
      };

      map<builtin_protocol_feature_t, std::filesystem::path>  found_builtin_protocol_features;
      map<digest_type, std::pair<builtin_protocol_feature, bool> > builtin_protocol_features_to_add;
      // The bool in the pair is set to true if the builtin protocol feature has already been visited to add
      map< builtin_protocol_feature_t, std::optional<digest_type> > visited_builtins;

      // Read all builtin protocol features
      if( directory_exists ) {
         for( directory_iterator enditr, itr{p}; itr != enditr; ++itr ) {
            auto file_path = itr->path();
            if( !std::filesystem::is_regular_file( file_path ) || file_path.extension().generic_string().compare( ".json" ) != 0 )
               continue;

            auto f = read_builtin_protocol_feature( file_path );

            if( !f ) continue;

            auto res = found_builtin_protocol_features.emplace( f->get_codename(), file_path );

            SYS_ASSERT( res.second, plugin_exception,
                        "Builtin protocol feature '${codename}' was already included from a previous_file",
                        ("codename", builtin_protocol_feature_codename(f->get_codename()))
                              ("current_file", file_path)
                              ("previous_file", res.first->second)
            );

            const auto feature_digest = f->digest();

            builtin_protocol_features_to_add.emplace( std::piecewise_construct,
                                                      std::forward_as_tuple( feature_digest ),
                                                      std::forward_as_tuple( *f, false ) );
         }
      }

      // Add builtin protocol features to the protocol feature manager in the right order (to satisfy dependencies)
      using itr_type = map<digest_type, std::pair<builtin_protocol_feature, bool>>::iterator;
      std::function<void(const itr_type&)> add_protocol_feature =
            [&pfs, &builtin_protocol_features_to_add, &visited_builtins, &log_recognized_protocol_feature, &add_protocol_feature]( const itr_type& itr ) -> void {
               if( itr->second.second ) {
                  return;
               } else {
                  itr->second.second = true;
                  visited_builtins.emplace( itr->second.first.get_codename(), itr->first );
               }

               for( const auto& d : itr->second.first.dependencies ) {
                  auto itr2 = builtin_protocol_features_to_add.find( d );
                  if( itr2 != builtin_protocol_features_to_add.end() ) {
                     add_protocol_feature( itr2 );
                  }
               }

               pfs.add_feature( itr->second.first );

               log_recognized_protocol_feature( itr->second.first, itr->first );
            };

      for( auto itr = builtin_protocol_features_to_add.begin(); itr != builtin_protocol_features_to_add.end(); ++itr ) {
         add_protocol_feature( itr );
      }

      auto output_protocol_feature = [&p]( const builtin_protocol_feature& f, const digest_type& feature_digest ) {
         string filename( "BUILTIN-" );
         filename += builtin_protocol_feature_codename( f.get_codename() );
         filename += ".json";

         auto file_path = p / filename;

         SYS_ASSERT( !std::filesystem::exists( file_path ), plugin_exception,
                     "Could not save builtin protocol feature with codename '${codename}' because a file at the following path already exists: ${path}",
                     ("codename", builtin_protocol_feature_codename( f.get_codename() ))
                           ("path", file_path)
         );

         if( fc::json::save_to_file( f, file_path ) ) {
            ilog( "Saved default specification for builtin protocol feature '${codename}' (with digest of '${digest}') to: ${path}",
                  ("codename", builtin_protocol_feature_codename(f.get_codename()))
                        ("digest", feature_digest)
                        ("path", file_path)
            );
         } else {
            elog( "Error occurred while writing default specification for builtin protocol feature '${codename}' (with digest of '${digest}') to: ${path}",
                  ("codename", builtin_protocol_feature_codename(f.get_codename()))
                        ("digest", feature_digest)
                        ("path", file_path)
            );
         }
      };

      std::function<digest_type(builtin_protocol_feature_t)> add_missing_builtins =
            [&pfs, &visited_builtins, &output_protocol_feature, &log_recognized_protocol_feature, &add_missing_builtins, populate_missing_builtins]
                  ( builtin_protocol_feature_t codename ) -> digest_type {
               auto res = visited_builtins.emplace( codename, std::optional<digest_type>() );
               if( !res.second ) {
                  SYS_ASSERT( res.first->second, protocol_feature_exception,
                              "invariant failure: cycle found in builtin protocol feature dependencies"
                  );
                  return *res.first->second;
               }

               auto f = protocol_feature_set::make_default_builtin_protocol_feature( codename,
                                                                                     [&add_missing_builtins]( builtin_protocol_feature_t d ) {
                                                                                        return add_missing_builtins( d );
                                                                                     } );

               if( !populate_missing_builtins )
                  f.subjective_restrictions.enabled = false;

               const auto& pf = pfs.add_feature( f );
               res.first->second = pf.feature_digest;

               log_recognized_protocol_feature( f, pf.feature_digest );

               if( populate_missing_builtins )
                  output_protocol_feature( f, pf.feature_digest );

               return pf.feature_digest;
            };

      for( const auto& p : builtin_protocol_feature_codenames ) {
         auto itr = found_builtin_protocol_features.find( p.first );
         if( itr != found_builtin_protocol_features.end() ) continue;

         add_missing_builtins( p.first );
      }

      return pfs;
   }


} }  // sysio::chain
