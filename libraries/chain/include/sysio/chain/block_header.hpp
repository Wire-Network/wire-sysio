#pragma once
#include <sysio/chain/block_timestamp.hpp>
#include <sysio/chain/producer_schedule.hpp>
#include <sysio/chain/protocol_feature_activation.hpp>
#include <sysio/chain/s_root_extension.hpp>

#include <type_traits>

namespace sysio { namespace chain {

   namespace detail {
      template<typename... Ts>
      struct block_header_extension_types {
         using block_header_extension_t = std::variant< Ts... >;
         using decompose_t = decompose< Ts... >;
      };
   }

   using block_header_extension_types = detail::block_header_extension_types<
      protocol_feature_activation,
      producer_schedule_change_extension,
      s_root_extension
   >;

   using block_header_extension = block_header_extension_types::block_header_extension_t;

   struct block_header
   {
      block_timestamp_type             timestamp;
      account_name                     producer;

      /**
       *  By signing this block this producer is confirming blocks [block_num() - confirmed, blocknum())
       *  as being the best blocks for that range and that he has not signed any other
       *  statements that would contradict.
       *
       *  No producer should sign a block with overlapping ranges or it is proof of byzantine
       *  behavior. When producing a block a producer is always confirming at least the block he
       *  is building off of.  A producer cannot confirm "this" block, only prior blocks.
       */
      uint16_t                         confirmed = 1;

      block_id_type                    previous;

      checksum256_type                 transaction_mroot; /// mroot of cycles_summary
      checksum256_type                 action_mroot; /// mroot of all delivered action receipts
      uint32_t                         schedule_version = 0;

      /**
       * LEGACY SUPPORT - After enabling the wtmsig-blocks extension this field is deprecated and must be empty
       * Wire uses wtmsig-blocks extension from genesis. This member is kept to provide binary compatibility
       * of block_header with other AntelopeIO chains.
       */
      using new_producers_type = std::optional<legacy::producer_schedule_type>;
      // old `new_producers` which is not used by wire as wtmsig-blocks extension is used from genesis.
      new_producers_type                not_used;
      extensions_type                   header_extensions;


      block_header() = default;

      digest_type       digest()const;
      block_id_type     calculate_id() const;
      uint32_t          block_num() const { return num_from_id(previous) + 1; }
      static uint32_t   num_from_id(const block_id_type& id);

      flat_multimap<uint16_t, block_header_extension> validate_and_extract_header_extensions()const;
   };


   struct signed_block_header : public block_header
   {
      signature_type    producer_signature;
   };

} } /// namespace sysio::chain

FC_REFLECT(sysio::chain::block_header,
           (timestamp)(producer)(confirmed)(previous)
           (transaction_mroot)(action_mroot)
           (schedule_version)(not_used)(header_extensions))

FC_REFLECT_DERIVED(sysio::chain::signed_block_header, (sysio::chain::block_header), (producer_signature))
