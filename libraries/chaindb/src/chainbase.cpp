#include <chainbase/chainbase.hpp>
#include <boost/array.hpp>

#include <iostream>

#ifndef _WIN32
#include <sys/mman.h>
#endif

namespace chainbase {

   database::database(const std::filesystem::path& dir, open_flags flags, uint64_t shared_file_size, bool allow_dirty,
                      pinnable_mapped_file::map_mode db_map_mode) :
      _db_file(dir, flags & database::read_write, shared_file_size, allow_dirty, db_map_mode),
      _read_only(flags == database::read_only)
   {
      _read_only_mode = _read_only;
   }

   database::~database()
   {
      _index_list.clear();
      _index_map.clear();
   }

   void database::undo()
   {
      if ( _read_only_mode )
         BOOST_THROW_EXCEPTION( std::logic_error( "attempting to undo in read-only mode" ) );
      undo_from_session();
   }

   void database::squash()
   {
      if ( _read_only_mode )
         BOOST_THROW_EXCEPTION( std::logic_error( "attempting to squash in read-only mode" ) );
      squash_from_session();
   }

   void database::undo_from_session()
   {
      for( auto& item : _index_list )
         item->undo();
   }

   void database::squash_from_session()
   {
      for( auto& item : _index_list )
         item->squash();
   }

   void database::commit( int64_t revision )
   {
      if ( _read_only_mode )
         BOOST_THROW_EXCEPTION( std::logic_error( "attempting to commit in read-only mode" ) );
      for( auto& item : _index_list )
      {
         item->commit( revision );
      }
   }

   void database::undo_all()
   {
      if ( _read_only_mode )
         BOOST_THROW_EXCEPTION( std::logic_error( "attempting to undo_all in read-only mode" ) );
      for( auto& item : _index_list )
      {
         item->undo_all();
      }
   }

   database::session database::start_undo_session( bool enabled )
   {
      if ( _read_only_mode )
         BOOST_THROW_EXCEPTION( std::logic_error( "attempting to start_undo_session in read-only mode" ) );
      if( enabled ) {
         // add_undo_session() allocates and can throw partway through the list. No session object
         // exists yet to unwind them, so roll back the indexes already pushed; otherwise indexes
         // end up at different undo-stack depths and later undo/squash/commit corrupts state.
         // Only the indexes pushed so far are undone: undoing all of them would pop a parent
         // session from the indexes that never received a new one. undo() is noexcept and pops
         // the just-added (empty) session.
         size_t added = 0;
         try {
            for( ; added < _index_list.size(); ++added )
               _index_list[added]->add_undo_session();
         } catch( ... ) {
            while( added > 0 )
               _index_list[--added]->undo();
            throw;
         }
         return session( *this );
      } else {
         return session();
      }
   }

}  // namespace chainbase
