#pragma once
#include <sysio/chain/block_timestamp.hpp>
#include <sysio/chain/protocol_feature_activation.hpp>
#include <sysio/chain/s_root_extension.hpp>
#include <sysio/chain/finality_core.hpp>
#include <sysio/chain/finalizer_policy.hpp>
#include <sysio/chain/proposer_policy.hpp>

#include <optional>
#include <type_traits>

namespace sysio::chain {

   namespace detail {
      template<typename... Ts>
      struct block_header_extension_types {
         using block_header_extension_t = std::variant< Ts... >;
         using decompose_t = decompose< Ts... >;
      };
   }

   using block_header_extension_types = detail::block_header_extension_types<
      protocol_feature_activation,
      s_root_extension
   >;

   using block_header_extension = block_header_extension_types::block_header_extension_t;
   using header_extension_multimap = flat_multimap<uint16_t, block_header_extension>;

   using validator_t = const std::function<void(block_timestamp_type, const flat_set<digest_type>&, const vector<digest_type>&)>;

   struct block_header
   {
      block_timestamp_type                  timestamp;
      account_name                          producer;

      block_id_type                         previous;

      checksum256_type                      transaction_mroot; /// mroot of cycles_summary

      // Root of the Finality Tree associated with the block,
      // i.e. the root of validation_tree(core.latest_qc_claim().block_num).
      checksum256_type                      finality_mroot;

      // Finality fields
      qc_claim_t                            qc_claim;
      std::optional<finalizer_policy_diff>  new_finalizer_policy_diff;
      std::optional<proposer_policy_diff>   new_proposer_policy_diff;

      extensions_type                       header_extensions;

      digest_type       digest()const;
      block_id_type     calculate_id() const;
      uint32_t          block_num() const { return num_from_id(previous) + 1; }
      static uint32_t   num_from_id(const block_id_type& id);
      uint32_t          protocol_version() const { return 0; }

      header_extension_multimap validate_and_extract_header_extensions()const;
      std::optional<block_header_extension> extract_header_extension(uint16_t extension_id)const;
      template<typename Ext> Ext extract_header_extension()const {
         assert(contains_header_extension(Ext::extension_id()));
         return std::get<Ext>(*extract_header_extension(Ext::extension_id()));
      }
      bool contains_header_extension(uint16_t extension_id)const;
   };


   struct signed_block_header : public block_header
   {
      std::vector<signature_type> producer_signatures;
   };

} /// namespace sysio::chain


FC_REFLECT(sysio::chain::block_header,
           (timestamp)(producer)(previous)
           (transaction_mroot)(finality_mroot)
           (qc_claim)(new_finalizer_policy_diff)(new_proposer_policy_diff)
           (header_extensions))

FC_REFLECT_DERIVED(sysio::chain::signed_block_header, (sysio::chain::block_header), (producer_signatures))
