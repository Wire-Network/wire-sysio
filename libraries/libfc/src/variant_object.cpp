#include <fc/variant_object.hpp>
#include <fc/exception/exception.hpp>
#include <fc/io/json.hpp>
#include <fc/io/json_stream.hpp>

namespace fc
{
   namespace {
      // Shared empty entries used by every default-constructed
      // variant_object / mutable_variant_object whose backing vector has
      // not yet been allocated.  begin() / end() / find() / size() /
      // operator[] route through this so they never have to branch on a
      // null _key_value, and the singleton is never mutated -- mutating
      // paths always allocate a fresh vector before writing.  Returned
      // as a non-const reference so the iterator type matches both
      // variant_object::iterator (a const_iterator) and
      // mutable_variant_object::iterator (a non-const iterator);
      // dereferencing an empty-range iterator is UB regardless, so the
      // static is read-only in practice.
      std::vector<variant_object::entry>& empty_entries() {
         static std::vector<variant_object::entry> v;
         return v;
      }

      // Lazy-init helper: ensures the unique_ptr is non-null before a
      // mutating mvo operation.
      void ensure_kv(std::unique_ptr<std::vector<variant_object::entry>>& p) {
         if (!p) p = std::make_unique<std::vector<variant_object::entry>>();
      }
   }

   // ---------------------------------------------------------------
   // entry

   variant_object::entry::entry() {}
   variant_object::entry::entry( std::string k, variant v ) : _key(std::move(k)),_value(std::move(v)) {}
   variant_object::entry::entry( entry&& e )  noexcept : _key(std::move(e._key)),_value(std::move(e._value)) {}
   variant_object::entry::entry( const entry& e ) : _key(e._key),_value(e._value) {}
   variant_object::entry& variant_object::entry::operator=( const variant_object::entry& e )
   {
      if( this != &e )
      {
         _key = e._key;
         _value = e._value;
      }
      return *this;
   }
   variant_object::entry& variant_object::entry::operator=( variant_object::entry&& e ) noexcept {
      fc_swap( _key, e._key );
      fc_swap( _value, e._value );
      return *this;
   }

   const std::string& variant_object::entry::key()const
   {
      return _key;
   }

   const variant& variant_object::entry::value()const
   {
      return _value;
   }
   variant& variant_object::entry::value()
   {
      return _value;
   }

   void  variant_object::entry::set( variant v )
   {
      fc_swap( _value, v );
   }

   // ---------------------------------------------------------------
   // variant_object

   variant_object::iterator variant_object::begin() const
   {
      return _key_value ? _key_value->begin() : empty_entries().begin();
   }

   variant_object::iterator variant_object::end() const
   {
      return _key_value ? _key_value->end() : empty_entries().end();
   }

   variant_object::iterator variant_object::find( const std::string& key )const
   {
      return find( key.c_str() );
   }

   variant_object::iterator variant_object::find( const char* key )const
   {
      if( !_key_value ) return empty_entries().end();
      const auto e = _key_value->end();
      for( auto itr = _key_value->begin(); itr != e; ++itr )
      {
         if( itr->key() == key )
         {
            return itr;
         }
      }
      return e;
   }

   const variant& variant_object::operator[]( const std::string& key )const
   {
      return (*this)[key.c_str()];
   }

   const variant& variant_object::operator[]( const char* key )const
   {
      auto itr = find( key );
      if( itr != end() ) return itr->value();
      FC_THROW_EXCEPTION( key_not_found_exception, "Key {}", key );
   }

   const variant& variant_object::find_or( const char* key, const variant& default_value ) const
   {
      // Single scan, no contains-then-op[] double-traversal, and no
      // throw-on-miss.  Reuses find() so the empty-singleton path in
      // the null _key_value case is correct.
      auto itr = find( key );
      return itr != end() ? itr->value() : default_value;
   }

   size_t variant_object::size() const
   {
      return _key_value ? _key_value->size() : 0;
   }

   variant_object::variant_object() = default;

   variant_object::variant_object( std::string key, variant val )
      : _key_value(std::make_shared<std::vector<entry>>())
   {
       _key_value->emplace_back(entry(std::move(key), std::move(val)));
   }

   variant_object::variant_object( const variant_object& obj )
   :_key_value( obj._key_value )
   {
   }

   variant_object::variant_object( variant_object&& obj) noexcept:
      _key_value( std::move(obj._key_value) )
   {
   }

   variant_object::variant_object( const mutable_variant_object& obj )
      : _key_value( obj._key_value
                       ? std::make_shared<std::vector<entry>>(*obj._key_value)
                       : nullptr )
   {
   }

   variant_object::variant_object( mutable_variant_object&& obj )
   : _key_value(std::move(obj._key_value))
   {
   }

   variant_object& variant_object::operator=( variant_object&& obj ) noexcept {
      if (this != &obj)
      {
         fc_swap(_key_value, obj._key_value );
      }
      return *this;
   }

   variant_object& variant_object::operator=( const variant_object& obj )
   {
      if (this != &obj)
      {
         _key_value = obj._key_value;
      }
      return *this;
   }

   variant_object& variant_object::operator=( mutable_variant_object&& obj )
   {
      _key_value = std::move(obj._key_value);
      return *this;
   }

   variant_object& variant_object::operator=( const mutable_variant_object& obj )
   {
      // Always detach: this fixes the prior aliasing bug where writing
      // through a shared shared_ptr could mutate a sibling variant_object
      // that had been copy-shared from this one.  And it gives the lazy
      // path the only allocation it needs.
      _key_value = obj._key_value
                     ? std::make_shared<std::vector<entry>>(*obj._key_value)
                     : nullptr;
      return *this;
   }

   size_t variant_object::estimated_size()const
   {
      auto kv_size = size();
      size_t sum = sizeof(*this) + sizeof(std::vector<entry>);
      for (size_t iter = 0; iter < kv_size; ++iter) {
         const auto& kv = (*_key_value)[iter];
         sum += kv.key().length() + sizeof(std::string);
         sum += kv.value().estimated_size();
      }
      return sum;
   }

   void to_variant( const variant_object& var,  variant& vo )
   {
      vo = variant(var);
   }

   void from_variant( const variant& var,  variant_object& vo )
   {
      vo = var.get_object();
   }

   // ---------------------------------------------------------------
   // mutable_variant_object

   mutable_variant_object::iterator mutable_variant_object::begin()
   {
      ensure_kv(_key_value);
      return _key_value->begin();
   }

   mutable_variant_object::iterator mutable_variant_object::end()
   {
      ensure_kv(_key_value);
      return _key_value->end();
   }

   mutable_variant_object::iterator mutable_variant_object::begin() const
   {
      return _key_value ? _key_value->begin() : empty_entries().begin();
   }

   mutable_variant_object::iterator mutable_variant_object::end() const
   {
      return _key_value ? _key_value->end() : empty_entries().end();
   }

   mutable_variant_object::iterator mutable_variant_object::find( const std::string& key )const
   {
      return find( key.c_str() );
   }

   mutable_variant_object::iterator mutable_variant_object::find( const char* key )const
   {
      if( !_key_value ) return empty_entries().end();
      const auto e = _key_value->end();
      for( auto itr = _key_value->begin(); itr != e; ++itr )
      {
         if( itr->key() == key )
         {
            return itr;
         }
      }
      return e;
   }

   mutable_variant_object::iterator mutable_variant_object::find( const std::string& key )
   {
      return find( key.c_str() );
   }

   mutable_variant_object::iterator mutable_variant_object::find( const char* key )
   {
      ensure_kv(_key_value);
      const auto e = _key_value->end();
      for( auto itr = _key_value->begin(); itr != e; ++itr )
      {
         if( itr->key() == key )
         {
            return itr;
         }
      }
      return e;
   }

   bool   mutable_variant_object::contains( const char* key )  { return find(key) != end(); }
   bool mutable_variant_object::contains(const std::string& key) {
      return contains(key.c_str());
   };
   bool mutable_variant_object::contains(const char* key) const {
      // Reuse the const find() (empty-singleton path) so a missing-key probe on a
      // default-constructed mvo doesn't allocate the entry vector.
      return std::as_const(*this).find(key) != std::as_const(*this).end();
   }
   bool mutable_variant_object::contains(const std::string& key) const {
      return contains(key.c_str());
   }

   const variant& mutable_variant_object::operator[]( const std::string& key )const
   {
      return (*this)[key.c_str()];
   }

   const variant& mutable_variant_object::operator[]( const char* key )const
   {
      auto itr = find( key );
      if( itr != end() ) return itr->value();
      FC_THROW_EXCEPTION( key_not_found_exception, "Key {}", key );
   }
   variant& mutable_variant_object::operator[]( const std::string& key )
   {
      return (*this)[key.c_str()];
   }

   variant& mutable_variant_object::operator[]( const char* key )
   {
      auto itr = find( key );
      if( itr != end() ) return itr->value();
      // find() with a non-const this already ensure_kv'd, so _key_value
      // is guaranteed non-null here.
      _key_value->emplace_back(entry(key, variant()));
      return _key_value->back().value();
   }

   size_t mutable_variant_object::size() const
   {
      return _key_value ? _key_value->size() : 0;
   }

   mutable_variant_object::mutable_variant_object() = default;

   mutable_variant_object::mutable_variant_object( std::string key, variant val )
      : _key_value(new std::vector<entry>())
   {
       _key_value->push_back(entry(std::move(key), std::move(val)));
   }

   mutable_variant_object::mutable_variant_object( const variant_object& obj )
      : _key_value( obj._key_value
                       ? new std::vector<entry>(*obj._key_value)
                       : nullptr )
   {
   }

   mutable_variant_object::mutable_variant_object( variant_object&& obj )
   {
      assert(obj._key_value.use_count() <= 1); // should only be used if data not shared
      if (!obj._key_value) return; // empty source -> stay lazy
      _key_value = std::make_unique<std::vector<entry>>();
      if (obj._key_value.use_count() == 1)
         *_key_value = std::move(*obj._key_value);
      else
         *_key_value = *obj._key_value;
   }

   mutable_variant_object::mutable_variant_object( const mutable_variant_object& obj )
      : _key_value( obj._key_value
                       ? new std::vector<entry>(*obj._key_value)
                       : nullptr )
   {
   }

   mutable_variant_object::mutable_variant_object( mutable_variant_object&& obj ) noexcept
      : _key_value(std::move(obj._key_value))
   {
   }

   mutable_variant_object& mutable_variant_object::operator=( const variant_object& obj )
   {
      _key_value = obj._key_value
                     ? std::make_unique<std::vector<entry>>(*obj._key_value)
                     : nullptr;
      return *this;
   }

   mutable_variant_object& mutable_variant_object::operator=( variant_object&& obj )
   {
      assert(obj._key_value.use_count() <= 1); // should only be used if data not shared
      if (!obj._key_value) {
         _key_value.reset();
         return *this;
      }
      ensure_kv(_key_value);
      if (obj._key_value.use_count() == 1)
         *_key_value = std::move(*obj._key_value);
      else
         *_key_value = *obj._key_value;
      return *this;
   }

   mutable_variant_object& mutable_variant_object::operator=( mutable_variant_object&& obj ) noexcept
   {
      if (this != &obj)
      {
         _key_value = std::move(obj._key_value);
      }
      return *this;
   }

   mutable_variant_object& mutable_variant_object::operator=( const mutable_variant_object& obj )
   {
      if (this != &obj)
      {
         _key_value = obj._key_value
                        ? std::make_unique<std::vector<entry>>(*obj._key_value)
                        : nullptr;
      }
      return *this;
   }

   void mutable_variant_object::reserve( size_t s )
   {
      ensure_kv(_key_value);
      _key_value->reserve(s);
   }

   void  mutable_variant_object::erase( const std::string& key )
   {
      if (!_key_value) return;
      for( auto itr = _key_value->begin(); itr != _key_value->end(); ++itr )
      {
         if( itr->key() == key )
         {
            _key_value->erase(itr);
            return;
         }
      }
   }

   /** replaces the value at \a key with \a var or insert's \a key if not found */
   mutable_variant_object& mutable_variant_object::set( std::string key, variant var ) &
   {
      ensure_kv(_key_value);
      auto itr = find( key.c_str() );
      if( itr != end() )
      {
         itr->set( std::move(var) );
      }
      else
      {
         _key_value->push_back( entry( std::move(key), std::move(var) ) );
      }
      return *this;
   }

   mutable_variant_object mutable_variant_object::set( std::string key, variant var ) &&
   {
      ensure_kv(_key_value);
      auto itr = find( key.c_str() );
      if( itr != end() )
      {
         itr->set( std::move(var) );
      }
      else
      {
         _key_value->push_back( entry( std::move(key), std::move(var) ) );
      }
      return std::move(*this);
   }

   /** Appends \a key and \a var without checking for duplicates, designed to
    *  simplify construction of dictionaries using (key,val)(key2,val2) syntax
    */
   mutable_variant_object& mutable_variant_object::operator()( std::string key, variant var ) &
   {
      ensure_kv(_key_value);
      _key_value->push_back( entry( std::move(key), std::move(var) ) );
      return *this;
   }

   mutable_variant_object mutable_variant_object::operator()( std::string key, variant var ) &&
   {
      ensure_kv(_key_value);
      _key_value->push_back( entry( std::move(key), std::move(var) ) );
      return std::move(*this);
   }

   mutable_variant_object& mutable_variant_object::operator()( const variant_object& vo ) &
   {
      for( const variant_object::entry& e : vo )
         set( e.key(), e.value() );
      return *this;
   }

   mutable_variant_object mutable_variant_object::operator()( const variant_object& vo ) &&
   {
      for( const variant_object::entry& e : vo )
         set( e.key(), e.value() );
      return std::move(*this);
   }

   mutable_variant_object& mutable_variant_object::operator()( const mutable_variant_object& mvo ) &
   {
      if( &mvo == this )     // mvo(mvo) is no-op
         return *this;
      for( const mutable_variant_object::entry& e : mvo )
         set( e.key(), e.value() );
      return *this;
   }

   mutable_variant_object mutable_variant_object::operator()( const mutable_variant_object& mvo ) &&
   {
      for( const mutable_variant_object::entry& e : mvo )
         set( e.key(), e.value() );
      return std::move(*this);
   }

   void to_variant( const mutable_variant_object& var,  variant& vo )
   {
      vo = variant(var);
   }

   void to_json_stream( const variant_object& vo, json_writer& w )
   {
      // Walk the object's key/value pairs directly into json_writer.  Mirrors the
      // compact (indent=0) form of fc::json::to_string for byte-identical output;
      // values recurse into to_json_stream(variant, w) which handles nested arrays /
      // objects without going through fc::json::to_string at any level.
      w.begin_object();
      for( const auto& kv : vo ) {
         w.key( kv.key() );
         to_json_stream( kv.value(), w );
      }
      w.end_object();
   }

   void to_json_stream( const mutable_variant_object& vo, json_writer& w )
   {
      // mutable_variant_object iterates the same pair shape as variant_object.
      w.begin_object();
      for( const auto& kv : vo ) {
         w.key( kv.key() );
         to_json_stream( kv.value(), w );
      }
      w.end_object();
   }

   void from_variant( const variant& var,  mutable_variant_object& vo )
   {
      vo = var.get_object();
   }

} // namesapce fc
