#pragma once
#include <sysio/chain/block_header.hpp>
#include <sysio/chain/transaction.hpp>

namespace sysio { namespace chain {

   struct transaction_receipt_header {
      enum status_enum : uint8_t { // Used for backward compatibility of SHiP and TraceAPI
         executed  = 0 ///< succeed
      };
      bool operator==(const transaction_receipt_header&) const = default;

      cpu_usage_t    cpu_usage_us; ///< billed CPU for nth input action
   };

   struct transaction_receipt : public transaction_receipt_header {

      transaction_receipt() = default;
      explicit transaction_receipt( const packed_transaction& ptrx ):trx(ptrx){}
      packed_transaction trx;

      digest_type digest()const {
         digest_type::encoder enc;
         fc::raw::pack( enc, cpu_usage_us );
         fc::raw::pack( enc, trx.digest() );
         return enc.result();
      }
   };

   struct additional_block_signatures_extension : fc::reflect_init {
      static constexpr uint16_t extension_id() { return 2; }
      static constexpr bool     enforce_unique() { return true; }

      additional_block_signatures_extension() = default;

      additional_block_signatures_extension( const vector<signature_type>& signatures )
      :signatures( signatures )
      {}

      additional_block_signatures_extension( vector<signature_type>&& signatures )
      :signatures( std::move(signatures) )
      {}

      void reflector_init();

      vector<signature_type> signatures;
   };

   namespace detail {
      template<typename... Ts>
      struct block_extension_types {
         using block_extension_t = std::variant< Ts... >;
         using decompose_t = decompose< Ts... >;
      };
   }

   using block_extension_types = detail::block_extension_types<
         additional_block_signatures_extension
   >;

   using block_extension = block_extension_types::block_extension_t;

   /**
    */
   struct signed_block : public signed_block_header{
   private:
      signed_block( const signed_block& ) = default;
   public:
      signed_block() = default;
      explicit signed_block( const signed_block_header& h ):signed_block_header(h){}
      signed_block( signed_block&& ) = default;
      signed_block& operator=(const signed_block&) = delete;
      signed_block& operator=(signed_block&&) = default;
      signed_block clone() const { return *this; }

      deque<transaction_receipt>   transactions; /// new or generated transactions
      extensions_type               block_extensions;

      flat_multimap<uint16_t, block_extension> validate_and_extract_extensions()const;
   };
   using signed_block_ptr = std::shared_ptr<signed_block>;

   struct producer_confirmation {
      block_id_type   block_id;
      digest_type     block_digest;
      account_name    producer;
      signature_type  sig;
   };

} } /// sysio::chain

FC_REFLECT_ENUM( sysio::chain::transaction_receipt::status_enum,
                 (executed) )

FC_REFLECT(sysio::chain::transaction_receipt_header, (cpu_usage_us) )
FC_REFLECT_DERIVED(sysio::chain::transaction_receipt, (sysio::chain::transaction_receipt_header), (trx) )
FC_REFLECT(sysio::chain::additional_block_signatures_extension, (signatures));
FC_REFLECT_DERIVED(sysio::chain::signed_block, (sysio::chain::signed_block_header), (transactions)(block_extensions) )
