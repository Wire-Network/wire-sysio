#pragma once

#include <ios>
#include <fc/io/cfile.hpp>
#include <fc/crypto/elliptic_ed.hpp>
namespace sysio::trace_api {

   class compressed_file_datastream;
   struct compressed_file_impl;
   /**
    * wrapper for read-only access to a compressed file.
    * compressed files support seeking and reading
    *
    * the efficiency of seeking is lower than that of an uncompressed file as each seek translates to
    *  - 2 seeks + 1 read to load and process the seek-point-mapping
    *  - potentially a read/decompress/discard of the data between the seek point and the requested offset
    *
    * More seek points can lower the average amount of data that must be read/decompressed/discarded in order
    * to seek to any given offset.  However, each seek point has some effect on the file size as it represents a
    * flush of the compressor which can degrade compression performance.
    *
    *  A compressed file looks like this on the filesystem:
    * /====================\ file offset 0
    * |                    |
    * |  Compressed Data   |
    * |  with seek points  |
    * |                    |
    * |--------------------|  file offset END - 2 - (16 * seek point count)
    * |                    |
    * |  mapping of        |
    * |    orig offset to  |
    * |    seek pt offset  |
    * |                    |
    * |--------------------|  file offset END - 2
    * |  seek pt count     |
    * \====================/  file offset END
    *
    * Where a "seek point" is a point in the compressed data stream where
    * the decompressor can start reading from having not read any of the prior data
    * seek points should be traversable by a decompressor so that reads which span
    * seek points do not have to be aware of them
    *
    * In zlib this is created by doing a complete flush of the stream
    */
   class compressed_file {
   public:
      explicit compressed_file( std::filesystem::path file_path );
      ~compressed_file();

      /**
       * Provide default move construction/assignment
       */
      compressed_file( compressed_file&& );
      compressed_file& operator= ( compressed_file&& );


      /**
       * Open the underlying fc::cfile for reading
       */
      void open() {
         file_ptr = std::make_unique<fc::cfile>();
         file_ptr->set_file_path(file_path);
         file_ptr->open("rb");
      }

      /**
       * Query whether the underlying file is open or not
       *
       * @return true if the file is open
       */
      bool is_open() const { return (bool)file_ptr; }

      /**
       * Seek to the given uncompressed offset
       * @param loc the byte offset in the uncompressed file to seek to
       * @throws std::ios_base::failure if this would seek past the end of the file
       * @throws compressed_file_error if the compressed data stream is corrupt or unreadable
       */
      void seek( uint64_t loc );

      /**
       * Read a given number of uncompressed bytes to the buffer pointed to by `d`.
       *
       * This interface is made to match fc::cfile for easy integration
       *
       * @param d - buffer to write data to
       * @param n - the number of bytes to read
       * @throws std::ios_base::failure if this would result in reading past the end of the uncompressed file
       * @throws compressed_file_error if the compressed data stream is corrupt or unreadable
       */
      void read( char* d, size_t n );

      /**
       * Close the underlying fc::cfile
       */
      void close() {
         file_ptr.reset();
      }

      /**
       * return the file path associated with this compressed_file
       * @return the std::filesystem::path associated with this file
       */
      auto get_file_path() const {
         return file_path;
      }

      compressed_file_datastream create_datastream();

      /**
       * Convert the file that exists at `input_path` into a compressed_file written to `output_path`.
       *
       * @param input_path - the path to the input file
       * @param output_path - the path to write the output file to (overwriting an existing file at that path)
       * @param seek_point_stride - the number of uncompressed bytes between seek points
       * @return true if successful, false if there was no error but the process could not complete
       * @throws std::ios_base::failure if the input_path does not exist or the output_path cannot be written to
       * @throws compressed_file_error if there is an issue during compression of the data stream
       */
      static bool process( const std::filesystem::path& input_path, const std::filesystem::path& output_path, size_t seek_point_stride );

   private:
      std::filesystem::path file_path;
      std::unique_ptr<fc::cfile> file_ptr;
      std::unique_ptr<compressed_file_impl> impl;
   };

   /*
    *  @brief datastream adapter that adapts cfile for use with fc unpack
    *
    *  This class supports unpack functionality but not pack.
    */
   class compressed_file_datastream {
   public:
      explicit compressed_file_datastream( compressed_file& cf ) : cf(cf) {}

      void skip( size_t s ) {
         std::vector<char> d( s );
         read( &d[0], s );
      }

      bool read( char* d, size_t s ) {
         cf.read( d, s );
         return true;
      }

      bool get( unsigned char& c ) { return get( *(char*)&c ); }

      bool get( char& c ) { return read(&c, 1); }

   private:
      compressed_file& cf;
   };

   inline compressed_file_datastream compressed_file::create_datastream() {
      return compressed_file_datastream(*this);
   }

   /**
    * Typed exception to represent errors encountered due to the processing of a compressed file
    * and not the underlying fc::cfile access
    */
   class compressed_file_error : public std::runtime_error {
   public:
      using std::runtime_error::runtime_error;
   };

   inline compressed_file_datastream&
   operator>>( compressed_file_datastream& ds, fc::crypto::ed::public_key_shim& pk ) {
      ds.read(reinterpret_cast<char*>(pk._data.data), crypto_sign_PUBLICKEYBYTES);
      return ds;
   }

   inline compressed_file_datastream&
   operator>>( compressed_file_datastream& ds, fc::crypto::ed::signature_shim& sig ) {
      ds.read(reinterpret_cast<char*>(sig._data.data), crypto_sign_BYTES);
      // zero out the padding byte
      sig._data.data[crypto_sign_BYTES] = 0;
      return ds;
   }

   inline compressed_file_datastream&
   operator>>( compressed_file_datastream& ds, fc::crypto::ed::private_key_shim& sk ) {
      ds.read(reinterpret_cast<char*>(sk._data.data), crypto_sign_SECRETKEYBYTES);
      return ds;
   }
}
