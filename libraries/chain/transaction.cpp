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

   auto to_pk_str = [&](const auto& pk){ try { return pk.to_string([&]() {FC_CHECK_DEADLINE(deadline); }); } catch (...) { return std::string("unknown"); } };
   if( !allow_duplicate_keys ) {
      flat_set<public_key_type> seen;
      for( auto& pk : ed_pubkeys ) {
         auto [it, inserted] = seen.emplace(pk);
         SYS_ASSERT( inserted, tx_duplicate_sig, "duplicate ED public-key extension for key {}", to_pk_str(pk) );
      }
   }

   // Prepare index for public key extensions.
   size_t pubkey_idx = 0;

   if ( !signatures.empty() ) {
      const digest_type digest = sig_digest(chain_id, cfd);

      for ( auto const& sig : signatures ) {
            auto now = fc::time_point::now();
            SYS_ASSERT( now < deadline, tx_cpu_usage_exceeded,
                        "sig verification timed out {}us", now-start );

            // dynamic dispatch into the correct path
            sig.visit([&](auto const& shim){
               using Shim = std::decay_t<decltype(shim)>;

               if constexpr ( std::is_same_v<Shim, fc::crypto::bls::signature_shim>) {
                  SYS_THROW(fc::unsupported_exception, "BLS signatures can not be used to recover public keys.");
               } else if constexpr( Shim::is_recoverable ) {
                  // If public key can be recovered from signature
                  auto [itr, ok] = recovered_pub_keys.emplace(sig, digest);
                  SYS_ASSERT( allow_duplicate_keys || ok, tx_duplicate_sig, "duplicate signature for key {}", to_pk_str(*itr) );
               } else {
                  // If public key cannot be recovered from signature, we need to get it from transaction extensions and use verify.
                  SYS_ASSERT( pubkey_idx < ed_pubkeys.size(), unsatisfied_authorization, "missing ED pubkey extension for signature #{}", pubkey_idx );

                  const auto& pubkey   = ed_pubkeys[pubkey_idx++];
                  const auto& pubkey_shim = pubkey.template get<typename Shim::public_key_type>();

                  SYS_ASSERT( shim.verify(digest, pubkey_shim), unsatisfied_authorization, "non-recoverable signature #{} failed", pubkey_idx-1 );

                  recovered_pub_keys.emplace(pubkey);
               }
            });
      }
   }

   // Ensure no extra ED pubkey extensions were provided
   SYS_ASSERT( pubkey_idx == ed_pubkeys.size(), unsatisfied_authorization,
               "got {} ED public-key extensions but only {} ED signatures", ed_pubkeys.size(), pubkey_idx );

   return fc::time_point::now() - start;
} FC_CAPTURE_AND_RETHROW("") }

account_name transaction::first_authorizer()const {
   for( const auto& a : actions ) {
      for( const auto& u : a.authorization )
         return u.actor;
   }
   return account_name();
}

action_payers_t transaction::first_authorizers()const {
   action_payers_t result;
   for (const auto& a : context_free_actions) {
      auto auth = a.first_authorizer();
      if (!auth.empty())
         result.insert(auth);
   }
   for (const auto& a : actions) {
      auto auth = a.first_authorizer();
      if (!auth.empty())
         result.insert(auth);
   }
   return result;
}

action_payers_t transaction::payers()const {
   action_payers_t result;
   for (const auto& a : context_free_actions) {
      result.insert(a.payer());
   }
   for (const auto& a : actions) {
      result.insert(a.payer());
   }
   return result;
}

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
                  "Transaction extension with id type {} is not supported", id
      );

      if( match->enforce_unique ) {
         SYS_ASSERT( i == 0 || id > id_type_lower_bound, invalid_transaction_extension,
                     "Transaction extension with id type {} is not allowed to repeat", id
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

uint32_t packed_transaction::get_action_billable_size(size_t action_index)const {
   assert(action_index < unpacked_trx.total_actions());
   assert(billable_net_per_action_overhead > 0);

   uint32_t size = billable_net_per_action_overhead;
   if (action_index < unpacked_trx.context_free_actions.size()) {
      // asserted to be less than or equal to context_free_actions.size()
      if (unpacked_trx.context_free_data.size() > action_index) {
         size += unpacked_trx.context_free_data[action_index].size();
      }
      size += unpacked_trx.context_free_actions[action_index].get_billable_size();
   } else {
      size += unpacked_trx.actions[action_index - unpacked_trx.context_free_actions.size()].get_billable_size();
   }
   return size;
}

size_t packed_transaction::get_estimated_size()const {
   // transaction is stored packed (only transaction minus signed_transaction members) and unpacked (signed_transaction),
   // double packed size, packed cfd size, and signature size to account for signed_transaction unpacked_trx size
   return sizeof(*this) +
          (signatures.size() * sizeof( signature_type )) * 2 +
          packed_context_free_data.size() * 2 +
          packed_trx.size() * 2;
}


digest_type packed_transaction::digest()const {
   digest_type::encoder enc;
   fc::raw::pack( enc, signatures );
   fc::raw::pack( enc, packed_context_free_data );
   // compression is `none` in consensus, so not necessary
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
   transaction trx;
   fc::datastream<const char*> ds(data.data(), data.size());
   fc::raw::unpack(ds, trx);
   SYS_ASSERT( !ds.remaining(), tx_extra_data, "packed_transaction contains extra data beyond transaction struct" );
   return trx;
}

static bytes zlib_decompress(const bytes& data) {
   try {
      bytes out;
      bio::filtering_ostream decomp;
      decomp.push(bio::zlib_decompressor());
      decomp.push(read_limiter<10*1024*1024>()); // limit to 10 meg decompressed for zip bomb protections
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
   init();
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
   init();
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
   init();
}

void packed_transaction::decompress() {
   switch (compression) {
      case compression_type::none:
         return;
      case compression_type::zlib:
         break;
      default:
         SYS_THROW(unknown_transaction_compression, "Unknown compression type");
   }
   packed_trx = zlib_decompress(packed_trx);
   if (!packed_context_free_data.empty()) {
      packed_context_free_data = zlib_decompress(packed_context_free_data);
   }
   compression = compression_type::none;
}

void packed_transaction::init()
{
   SYS_ASSERT( !unpacked_trx.actions.empty(), tx_no_action, "packed_transaction contains no actions" );
   SYS_ASSERT( unpacked_trx.context_free_data.empty() || unpacked_trx.context_free_data.size() == unpacked_trx.context_free_actions.size(), transaction_exception,
              "Context free data size {} not equal to context free actions size {}",
              unpacked_trx.context_free_data.size(), unpacked_trx.context_free_actions.size() );

   int64_t size = config::fixed_net_overhead_of_packed_trx;
   size += fc::raw::pack_size(signatures);
   size += fc::raw::pack_size(unpacked_trx.transaction_extensions);
   size += fc::raw::pack_size(static_cast<const transaction_header&>(unpacked_trx));
   SYS_ASSERT( size + packed_trx.size() + packed_context_free_data.size() <= std::numeric_limits<uint32_t>::max(),
               tx_too_big, "packed_transaction is too big" );
   billable_net_per_action_overhead = (size / unpacked_trx.total_actions()) + 1;
}

void packed_transaction::reflector_init()
{
   // called after construction, but always on the same thread and before packed_transaction passed to any other threads
   static_assert(fc::raw::has_feature_reflector_init_on_unpacked_reflected_types,
                 "FC unpack needs to call reflector_init otherwise unpacked_trx will not be initialized");
   SYS_ASSERT( unpacked_trx.expiration == time_point_sec(), tx_decompression_error, "packed_transaction already unpacked" );
   local_unpack_transaction({});
   local_unpack_context_free_data();
   decompress();
   init();
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
   } FC_CAPTURE_AND_RETHROW( "{}", compression.to_string() )
}

void packed_transaction::local_unpack_context_free_data()
{
   assert(!trx_id.empty()); // must call after local_unpack_transaction
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
   } FC_CAPTURE_AND_RETHROW( "{}", compression.to_string() )
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
   } FC_CAPTURE_AND_RETHROW( "{}", compression.to_string() )
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
   } FC_CAPTURE_AND_RETHROW( "{}", compression.to_string() )
}


} } // sysio::chain
