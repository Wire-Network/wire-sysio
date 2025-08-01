#include <fc/io/raw.hpp>
#include <fc/bitutil.hpp>
#include <algorithm>

#include <boost/range/adaptor/transformed.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/device/back_inserter.hpp>
#include <boost/iostreams/filter/zlib.hpp>

#include <sysio/chain/config.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/transaction.hpp>

#include <fc/static_variant.hpp>
#include <fc/crypto/elliptic_ed.hpp> 
#include <fc/crypto/signature.hpp>
namespace sysio { namespace chain {

void transaction_header::set_reference_block( const block_id_type& reference_block ) {
   ref_block_num    = fc::endian_reverse_u32(reference_block._hash[0]);
   ref_block_prefix = reference_block._hash[1];
}

bool transaction_header::verify_reference_block( const block_id_type& reference_block )const {
   return ref_block_num    == (decltype(ref_block_num))fc::endian_reverse_u32(reference_block._hash[0]) &&
          ref_block_prefix == (decltype(ref_block_prefix))reference_block._hash[1];
}

void transaction_header::validate()const {
   SYS_ASSERT( max_net_usage_words.value < UINT32_MAX / 8UL, transaction_exception,
               "declared max_net_usage_words overflows when expanded to max net usage" );
}

transaction_id_type transaction::id() const {
   digest_type::encoder enc;
   fc::raw::pack( enc, *this );
   return enc.result();
}

digest_type transaction::sig_digest( const chain_id_type& chain_id, const vector<bytes>& cfd )const {
   digest_type::encoder enc;
   fc::raw::pack( enc, chain_id );
   fc::raw::pack( enc, *this );
   if( cfd.size() ) {
      fc::raw::pack( enc, digest_type::hash(cfd) );
   } else {
      fc::raw::pack( enc, digest_type() );
   }
   return enc.result();
}

fc::microseconds transaction::get_signature_keys( const vector<signature_type>& signatures,
      const chain_id_type& chain_id, fc::time_point deadline, const vector<bytes>& cfd,
      flat_set<public_key_type>& recovered_pub_keys, bool allow_duplicate_keys)const
{ try {
   auto start = fc::time_point::now();
   recovered_pub_keys.clear();

   // 1) Extract and validate extensions
   auto validated_ext = validate_and_extract_extensions();
   vector<public_key_type> ed_pubkeys;
   for ( auto const& item : validated_ext ) {
      if ( auto* e = std::get_if<ed_pubkey_extension>(&item.second) ) {
         ed_pubkeys.emplace_back(e->pubkey);
      }
   }

   if( !allow_duplicate_keys ) {
      flat_set<public_key_type> seen;
      for( auto& pk : ed_pubkeys ) {
         auto [it, inserted] = seen.emplace(pk);
         SYS_ASSERT( inserted, tx_duplicate_sig, "duplicate ED public-key extension for key ${k}", ("k", pk) );
      }
   }

   // Prepare index for public key extensions.
   size_t pk_index = 0;

   if ( !signatures.empty() ) {
      const digest_type digest = sig_digest(chain_id, cfd);

      for ( auto const& sig : signatures ) {
            auto now = fc::time_point::now();
            SYS_ASSERT( now < deadline, tx_cpu_usage_exceeded,
                        "sig verification timed out ${t}us", ("t",now-start) );

            // dynamic dispatch into the correct path
            sig.visit([&](auto const& shim){
               using Shim = std::decay_t<decltype(shim)>;

               if constexpr( Shim::is_recoverable ) {
                  // If public key can be recovered from signature
                  auto [itr, ok] = recovered_pub_keys.emplace(sig, digest);
                  SYS_ASSERT( allow_duplicate_keys || ok, tx_duplicate_sig, "duplicate signature for key ${k}", ("k", *itr) );
               } else {
                  // If public key cannot be recovered from signature, we need to get it from transaction extensions and use verify.
                  SYS_ASSERT( pk_index < ed_pubkeys.size(), unsatisfied_authorization, "missing ED pubkey extension for signature #{i}", ("i", pk_index) );

                  const auto& pkvar   = ed_pubkeys[pk_index++];
                  const auto& pubshim = pkvar.template get<typename Shim::public_key_type>();

                  SYS_ASSERT( shim.verify(digest, pubshim), unsatisfied_authorization, "non-recoverable signature #${i} failed", ("i", pk_index-1) );

                  recovered_pub_keys.emplace(pkvar);
               }
            });
      }
   }

   // Ensure no extra ED pubkey extensions were provided
   SYS_ASSERT( pk_index == ed_pubkeys.size(), unsatisfied_authorization, "got ${g} ED public-key extensions but only ${e} ED signatures", ("g", ed_pubkeys.size()) ("e", pk_index) );

   return fc::time_point::now() - start;
} FC_CAPTURE_AND_RETHROW() }

flat_multimap<uint16_t, transaction_extension> transaction::validate_and_extract_extensions()const {
   using decompose_t = transaction_extension_types::decompose_t;

   flat_multimap<uint16_t, transaction_extension> results;

   uint16_t id_type_lower_bound = 0;

   for( size_t i = 0; i < transaction_extensions.size(); ++i ) {
      const auto& e = transaction_extensions[i];
      auto id = e.first;

      SYS_ASSERT( id >= id_type_lower_bound, invalid_transaction_extension,
                  "Transaction extensions are not in the correct order (ascending id types required)"
      );

      auto iter = results.emplace(std::piecewise_construct,
         std::forward_as_tuple(id),
         std::forward_as_tuple()
      );

      auto match = decompose_t::extract<transaction_extension>( id, e.second, iter->second );
      SYS_ASSERT( match, invalid_transaction_extension,
                  "Transaction extension with id type ${id} is not supported",
                  ("id", id)
      );

      if( match->enforce_unique ) {
         SYS_ASSERT( i == 0 || id > id_type_lower_bound, invalid_transaction_extension,
                     "Transaction extension with id type ${id} is not allowed to repeat",
                     ("id", id)
         );
      }

      id_type_lower_bound = id;
   }

   return results;
}

const signature_type& signed_transaction::sign(const private_key_type& key, const chain_id_type& chain_id) {
   signatures.push_back(key.sign(sig_digest(chain_id, context_free_data)));
   return signatures.back();
}

signature_type signed_transaction::sign(const private_key_type& key, const chain_id_type& chain_id)const {
   return key.sign(sig_digest(chain_id, context_free_data));
}

fc::microseconds
signed_transaction::get_signature_keys( const chain_id_type& chain_id, fc::time_point deadline,
                                        flat_set<public_key_type>& recovered_pub_keys,
                                        bool allow_duplicate_keys)const
{
   return transaction::get_signature_keys(signatures, chain_id, deadline, context_free_data, recovered_pub_keys, allow_duplicate_keys);
}

uint32_t packed_transaction::get_unprunable_size()const {
   uint64_t size = config::fixed_net_overhead_of_packed_trx;
   size += packed_trx.size();
   SYS_ASSERT( size <= std::numeric_limits<uint32_t>::max(), tx_too_big, "packed_transaction is too big" );
   return static_cast<uint32_t>(size);
}

uint32_t packed_transaction::get_prunable_size()const {
   uint64_t size = fc::raw::pack_size(signatures);
   size += packed_context_free_data.size();
   SYS_ASSERT( size <= std::numeric_limits<uint32_t>::max(), tx_too_big, "packed_transaction is too big" );
   return static_cast<uint32_t>(size);
}

size_t packed_transaction::get_estimated_size()const {
   // transaction is stored packed (only transaction minus signed_transaction members) and unpacked (signed_transaction),
   // double packed size, packed cfd size, and signature size to account for signed_transaction unpacked_trx size
   return sizeof(*this) +
          (signatures.size() * sizeof( signature_type )) * 2 +
          packed_context_free_data.size() * 2 +
          packed_trx.size() * 2;
}


digest_type packed_transaction::packed_digest()const {
   digest_type::encoder prunable;
   fc::raw::pack( prunable, signatures );
   fc::raw::pack( prunable, packed_context_free_data );

   digest_type::encoder enc;
   fc::raw::pack( enc, compression );
   fc::raw::pack( enc, packed_trx  );
   fc::raw::pack( enc, prunable.result() );

   return enc.result();
}

digest_type packed_transaction::digest()const {
   digest_type::encoder enc;
   fc::raw::pack( enc, signatures );
   fc::raw::pack( enc, packed_context_free_data );
   // compression is set by the node, so not actually necessary
   fc::raw::pack( enc, trx_id  );   // all of transaction is represented by trx id/digest

   return enc.result();
}

namespace bio = boost::iostreams;

template<size_t Limit>
struct read_limiter {
   using char_type = char;
   using category = bio::multichar_output_filter_tag;

   template<typename Sink>
   size_t write(Sink &sink, const char* s, size_t count)
   {
      SYS_ASSERT(_total + count <= Limit, tx_decompression_error, "Exceeded maximum decompressed transaction size");
      _total += count;
      return bio::write(sink, s, count);
   }

   size_t _total = 0;
};

static vector<bytes> unpack_context_free_data(const bytes& data) {
   if( data.size() == 0 )
      return vector<bytes>();

   return fc::raw::unpack< vector<bytes> >(data);
}

static transaction unpack_transaction(const bytes& data) {
   return fc::raw::unpack<transaction>(data);
}

static bytes zlib_decompress(const bytes& data) {
   try {
      bytes out;
      bio::filtering_ostream decomp;
      decomp.push(bio::zlib_decompressor());
      decomp.push(read_limiter<1*1024*1024>()); // limit to 1 meg decompressed for zip bomb protections
      decomp.push(bio::back_inserter(out));
      bio::write(decomp, data.data(), data.size());
      bio::close(decomp);
      return out;
   } catch( fc::exception& er ) {
      throw;
   } catch( ... ) {
      fc::unhandled_exception er( FC_LOG_MESSAGE( warn, "internal decompression error"), std::current_exception() );
      throw er;
   }
}

static vector<bytes> zlib_decompress_context_free_data(const bytes& data) {
   if( data.size() == 0 )
      return vector<bytes>();

   bytes out = zlib_decompress(data);
   return unpack_context_free_data(out);
}

static transaction zlib_decompress_transaction(const bytes& data) {
   bytes out = zlib_decompress(data);
   return unpack_transaction(out);
}

static bytes pack_transaction(const transaction& t) {
   return fc::raw::pack(t);
}

static bytes pack_context_free_data(const vector<bytes>& cfd ) {
   if( cfd.size() == 0 )
      return bytes();

   return fc::raw::pack(cfd);
}

static bytes zlib_compress_context_free_data(const vector<bytes>& cfd ) {
   if( cfd.size() == 0 )
      return bytes();

   bytes in = pack_context_free_data(cfd);
   bytes out;
   bio::filtering_ostream comp;
   comp.push(bio::zlib_compressor(bio::zlib::best_compression));
   comp.push(bio::back_inserter(out));
   bio::write(comp, in.data(), in.size());
   bio::close(comp);
   return out;
}

static bytes zlib_compress_transaction(const transaction& t) {
   bytes in = pack_transaction(t);
   bytes out;
   bio::filtering_ostream comp;
   comp.push(bio::zlib_compressor(bio::zlib::best_compression));
   comp.push(bio::back_inserter(out));
   bio::write(comp, in.data(), in.size());
   bio::close(comp);
   return out;
}

bytes packed_transaction::get_raw_transaction() const
{
   try {
      switch(compression) {
         case compression_type::none:
            return packed_trx;
         case compression_type::zlib:
            return zlib_decompress(packed_trx);
         default:
            SYS_THROW(unknown_transaction_compression, "Unknown transaction compression algorithm");
      }
   } FC_CAPTURE_AND_RETHROW((compression)(packed_trx))
}

packed_transaction::packed_transaction( bytes&& packed_txn, vector<signature_type>&& sigs, bytes&& packed_cfd, compression_type _compression )
:signatures(std::move(sigs))
,compression(_compression)
,packed_context_free_data(std::move(packed_cfd))
,packed_trx(std::move(packed_txn))
{
   local_unpack_transaction({});
   if( !packed_context_free_data.empty() ) {
      local_unpack_context_free_data();
   }
}

packed_transaction::packed_transaction( bytes&& packed_txn, vector<signature_type>&& sigs, vector<bytes>&& cfd, compression_type _compression )
:signatures(std::move(sigs))
,compression(_compression)
,packed_trx(std::move(packed_txn))
{
   local_unpack_transaction( std::move( cfd ) );
   if( !unpacked_trx.context_free_data.empty() ) {
      local_pack_context_free_data();
   }
}

packed_transaction::packed_transaction( transaction&& t, vector<signature_type>&& sigs, bytes&& packed_cfd, compression_type _compression )
:signatures(std::move(sigs))
,compression(_compression)
,packed_context_free_data(std::move(packed_cfd))
,unpacked_trx(std::move(t), signatures, {})
,trx_id(unpacked_trx.id())
{
   local_pack_transaction();
   if( !packed_context_free_data.empty() ) {
      local_unpack_context_free_data();
   }
}

void packed_transaction::reflector_init()
{
   // called after construction, but always on the same thread and before packed_transaction passed to any other threads
   static_assert(fc::raw::has_feature_reflector_init_on_unpacked_reflected_types,
                 "FC unpack needs to call reflector_init otherwise unpacked_trx will not be initialized");
   SYS_ASSERT( unpacked_trx.expiration == time_point_sec(), tx_decompression_error, "packed_transaction already unpacked" );
   local_unpack_transaction({});
   local_unpack_context_free_data();
}

void packed_transaction::local_unpack_transaction(vector<bytes>&& context_free_data)
{
   try {
      switch( compression ) {
         case compression_type::none:
            unpacked_trx = signed_transaction( unpack_transaction( packed_trx ), signatures, std::move(context_free_data) );
            break;
         case compression_type::zlib:
            unpacked_trx = signed_transaction( zlib_decompress_transaction( packed_trx ), signatures, std::move(context_free_data) );
            break;
         default:
            SYS_THROW( unknown_transaction_compression, "Unknown transaction compression algorithm" );
      }
      trx_id = unpacked_trx.id();
   } FC_CAPTURE_AND_RETHROW( (compression) )
}

void packed_transaction::local_unpack_context_free_data()
{
   try {
      SYS_ASSERT(unpacked_trx.context_free_data.empty(), tx_decompression_error, "packed_transaction.context_free_data not empty");
      switch( compression ) {
         case compression_type::none:
            unpacked_trx.context_free_data = unpack_context_free_data( packed_context_free_data );
            break;
         case compression_type::zlib:
            unpacked_trx.context_free_data = zlib_decompress_context_free_data( packed_context_free_data );
            break;
         default:
            SYS_THROW( unknown_transaction_compression, "Unknown transaction compression algorithm" );
      }
   } FC_CAPTURE_AND_RETHROW( (compression) )
}

void packed_transaction::local_pack_transaction()
{
   try {
      switch(compression) {
         case compression_type::none:
            packed_trx = pack_transaction(unpacked_trx);
            break;
         case compression_type::zlib:
            packed_trx = zlib_compress_transaction(unpacked_trx);
            break;
         default:
            SYS_THROW(unknown_transaction_compression, "Unknown transaction compression algorithm");
      }
   } FC_CAPTURE_AND_RETHROW((compression))
}

void packed_transaction::local_pack_context_free_data()
{
   try {
      switch(compression) {
         case compression_type::none:
            packed_context_free_data = pack_context_free_data(unpacked_trx.context_free_data);
            break;
         case compression_type::zlib:
            packed_context_free_data = zlib_compress_context_free_data(unpacked_trx.context_free_data);
            break;
         default:
            SYS_THROW(unknown_transaction_compression, "Unknown transaction compression algorithm");
      }
   } FC_CAPTURE_AND_RETHROW((compression))
}


} } // sysio::chain
