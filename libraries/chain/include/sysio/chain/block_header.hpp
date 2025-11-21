#pragma once
#include <sysio/chain/block_timestamp.hpp>
#include <sysio/chain/producer_schedule.hpp>
#include <sysio/chain/protocol_feature_activation.hpp>
#include <sysio/chain/finality_extension.hpp>
#include <sysio/chain/s_root_extension.hpp>

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
      producer_schedule_change_extension,
      s_root_extension,
      finality_extension
   >;

   using block_header_extension = block_header_extension_types::block_header_extension_t;
   using header_extension_multimap = flat_multimap<uint16_t, block_header_extension>;

   using validator_t = const std::function<void(block_timestamp_type, const flat_set<digest_type>&, const vector<digest_type>&)>;

   struct block_header
   {
      block_timestamp_type             timestamp;
      account_name                     producer;

      /**
       *  Legacy block confirmation:
       *  By signing this block this producer is confirming blocks [block_num() - confirmed, blocknum())
       *  as being the best blocks for that range and that he has not signed any other
       *  statements that would contradict.
       *
       *  No producer should sign a block with overlapping ranges or it is proof of byzantine
       *  behavior. When producing a block a producer is always confirming at least the block he
       *  is building off of.  A producer cannot confirm "this" block, only prior blocks.
       *
       *  Instant-finality:
       *  Once instant-finality is enabled a producer can no longer confirm blocks, only propose them;
       *  confirmed is 0 after instant-finality is enabled.
       */
      uint16_t                         confirmed = 1;

      block_id_type                    previous;

      checksum256_type                 transaction_mroot; /// mroot of cycles_summary

	  // In Legacy, action_mroot is the mroot of all delivered action receipts.
      // In Savanna, action_mroot is the root of the Finality Tree
      // associated with the block, i.e. the root of
      // validation_tree(core.latest_qc_claim().block_num).
      checksum256_type                 action_mroot;

      uint32_t                         schedule_version = 0;

      /**
       * LEGACY SUPPORT - After enabling the wtmsig-blocks extension this field is deprecated and must be empty
       *
       * Prior to that activation this carries:
       *
       * The producer schedule version that should validate this block, this is used to
       * indicate that the prior block which included new_producers->version has been marked
       * irreversible and that it the new producer schedule takes effect this block.
       *
       * Wire uses wtmsig-blocks extension from genesis. This member is kept to provide binary compatibility
       * of block_header with other AntelopeIO chains.
       */
      using new_producers_type = std::optional<legacy::producer_schedule_type>;
      // old `new_producers` which is not used by wire as wtmsig-blocks extension is used from genesis.
      new_producers_type                not_used;
      extensions_type                   header_extensions;

      digest_type       digest()const;
      block_id_type     calculate_id() const;
      uint32_t          block_num() const { return num_from_id(previous) + 1; }
      static uint32_t   num_from_id(const block_id_type& id);
      uint32_t          protocol_version() const { return 0; }

      // A flag to indicate whether a block is a Proper Savanna Block
      static constexpr uint32_t proper_svnn_schedule_version = (1LL << 31);

      // Returns true if the block is a Proper Savanna Block.
      // We don't check whether finality extension exists here for performance reason.
      // When block header is validated in block_header_state's next(),
      // it is already validate if schedule_version == proper_svnn_schedule_version,
      // finality extension must exist.
      bool is_proper_svnn_block() const { return ( schedule_version == proper_svnn_schedule_version ); }

      // Returns true if the block is a pure Legacy block
      bool is_legacy_block() const { return !contains_header_extension(finality_extension::extension_id()); }

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
      signature_type    producer_signature;
   };

} /// namespace sysio::chain


FC_REFLECT(sysio::chain::block_header,
           (timestamp)(producer)(confirmed)(previous)
           (transaction_mroot)(action_mroot)
           (schedule_version)(not_used)(header_extensions))

FC_REFLECT_DERIVED(sysio::chain::signed_block_header, (sysio::chain::block_header), (producer_signature))
