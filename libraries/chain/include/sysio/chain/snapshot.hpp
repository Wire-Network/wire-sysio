#pragma once

#include <sysio/chain/blake3_encoder.hpp>
#include <sysio/chain/database_utils.hpp>
#include <sysio/chain/exceptions.hpp>

#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <fc/io/raw.hpp>
#include <fc/io/random_access_file.hpp>
#include <fc/crypto/blake3.hpp>
#include <fc/crypto/elliptic_ed.hpp>
#include <fc/reflect/reflect.hpp>

#include <boost/core/demangle.hpp>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ostream>
#include <streambuf>


namespace sysio { namespace chain {
   /**
    * History:
    * Version 1: sequential write with parallel hash and index at end of file
    *
    * File format v1:
    *   [Header]  (8 bytes)
    *     magic:        uint32_t  (0x57495245 "WIRE")
    *     version:      uint32_t  (1)
    *
    *   [Section Data]
    *     section 0 raw packed rows
    *     section 1 raw packed rows
    *     ...
    *
    *   [Section Index] (num_sections entries, sorted by section name)
    *     name:         null-terminated string
    *     data_offset:  uint64_t  (from start of file)
    *     data_size:    uint64_t
    *     row_count:    uint64_t
    *     hash:         char[32]  (BLAKE3 of section row data)
    *
    *   [Footer]  (44 bytes)
    *     num_sections: uint32_t
    *     root_hash:    char[32]  (BLAKE3 of concatenated section hashes in canonical order)
    *     index_offset: uint64_t  (byte offset where section index starts)
    */
   static const uint32_t current_snapshot_version = 1;

   namespace detail {
      template<typename T>
      struct snapshot_section_traits {
         static std::string section_name() {
            return boost::core::demangle(typeid(T).name());
         }
      };

      template<typename T>
      struct snapshot_row_traits {
         using value_type = std::decay_t<T>;
         using snapshot_type = value_type;

         static const snapshot_type& to_snapshot_row( const value_type& value, const chainbase::database& ) {
            return value;
         };
      };

      /**
       * Due to a pattern in our code of overloading `operator << ( std::ostream&, ... )` to provide
       * human-readable string forms of data, we cannot directly use ostream as those operators will
       * be used instead of the expected operators.  In otherwords:
       * fc::raw::pack(fc::datastream...)
       * will end up calling _very_ different operators than
       * fc::raw::pack(std::ostream...)
       */
      struct ostream_wrapper {
         explicit ostream_wrapper(std::ostream& s)
         :inner(s) {

         }

         ostream_wrapper(ostream_wrapper &&) = default;
         ostream_wrapper(const ostream_wrapper& ) = default;

         auto& write( const char* d, size_t s ) {
            return inner.write(d, s);
         }

         auto& put(char c) {
           return inner.put(c);
         }

         auto tellp() const {
            return inner.tellp();
         }

         auto& seekp(std::ostream::pos_type p) {
            return inner.seekp(p);
         }

         std::ostream& inner;
      };


      struct abstract_snapshot_row_writer {
         virtual void write(ostream_wrapper& out) const = 0;
         virtual void write(fc::sha256::encoder& out) const = 0;
         virtual void write(blake3_encoder& out) const = 0;
         virtual fc::variant to_variant() const = 0;
         virtual std::string row_type_name() const = 0;
      };

      template<typename T>
      struct snapshot_row_writer : abstract_snapshot_row_writer {
         explicit snapshot_row_writer( const T& data )
         :data(data) {}

         template<typename DataStream>
         void write_stream(DataStream& out) const {
            fc::raw::pack(out, data);
         }

         void write(ostream_wrapper& out) const override {
            write_stream(out);
         }

         void write(fc::sha256::encoder& out) const override {
            write_stream(out);
         }

         void write(blake3_encoder& out) const override {
            write_stream(out);
         }

         fc::variant to_variant() const override {
            fc::variant var;
            fc::to_variant(data, var);
            return var;
         }

         std::string row_type_name() const override {
            return boost::core::demangle( typeid( T ).name() );
         }

         const T& data;
      };

      template<typename T>
      snapshot_row_writer<T> make_row_writer( const T& data) {
         return snapshot_row_writer<T>(data);
      }
   }

   class snapshot_writer {
      public:
         static constexpr uint32_t magic_number = 0x57495245; // WIRE in ASCII
         static constexpr uint32_t max_threads  = 4;

         class section_writer {
            public:
               template<typename T>
               void add_row( const T& row, const chainbase::database& db ) {
                  _writer.write_row(detail::make_row_writer(detail::snapshot_row_traits<T>::to_snapshot_row(row, db)));
               }

            private:
               friend class snapshot_writer;
               section_writer(snapshot_writer& writer)
               :_writer(writer)
               {

               }
               snapshot_writer& _writer;
         };

         template<typename F>
         void write_section(const std::string section_name, F f) {
            write_start_section(section_name);
            auto section = section_writer(*this);
            f(section);
            write_end_section();
         }

         template<typename T, typename F>
         void write_section(F f) {
            write_section(detail::snapshot_section_traits<T>::section_name(), f);
         }

         virtual ~snapshot_writer(){};

         virtual const char* name() const = 0;

      protected:
         virtual void write_start_section( const std::string& section_name ) = 0;
         virtual void write_row( const detail::abstract_snapshot_row_writer& row_writer ) = 0;
         virtual void write_end_section() = 0;
   };

   using snapshot_writer_ptr = std::shared_ptr<snapshot_writer>;

   namespace detail {
      struct abstract_snapshot_row_reader {
         virtual void provide(std::istream& in) const = 0;
         virtual void provide(const fc::variant&) const = 0;
         virtual void provide(fc::datastream<const char*>&) const = 0;
         virtual std::string row_type_name() const = 0;
      };

      template<typename T>
      struct is_chainbase_object {
         static constexpr bool value = false;
      };

      template<uint16_t TypeNumber, typename Derived>
      struct is_chainbase_object<chainbase::object<TypeNumber, Derived>> {
         static constexpr bool value = true;
      };

      template<typename T>
      constexpr bool is_chainbase_object_v = is_chainbase_object<T>::value;

      struct row_validation_helper {
         template<typename T, typename F>
         static auto apply(const T& data, F f) -> std::enable_if_t<is_chainbase_object_v<T>> {
            auto orig = data.id;
            f();
            SYS_ASSERT(orig == data.id, snapshot_exception,
                       "Snapshot for {} mutates row member \"id\" which is illegal",
                       boost::core::demangle( typeid( T ).name() ));
         }

         template<typename T, typename F>
         static auto apply(const T&, F f) -> std::enable_if_t<!is_chainbase_object_v<T>> {
            f();
         }
      };

      template<typename T>
      struct snapshot_row_reader : abstract_snapshot_row_reader {
         explicit snapshot_row_reader( T& data )
         :data(data) {}


         void provide(std::istream& in) const override {
            row_validation_helper::apply(data, [&in,this](){
               fc::raw::unpack(in, data);
            });
         }

         void provide(const fc::variant& var) const override {
            row_validation_helper::apply(data, [&var,this]() {
               fc::from_variant(var, data);
            });
         }

         void provide(fc::datastream<const char*>& ds) const override{
            row_validation_helper::apply(data, [&ds,this]() {
               fc::raw::unpack(ds, data);
            });
         }

         std::string row_type_name() const override {
            return boost::core::demangle( typeid( T ).name() );
         }

         T& data;
      };

      template<typename T>
      snapshot_row_reader<T> make_row_reader( T& data ) {
         return snapshot_row_reader<T>(data);
      }
   }

   class snapshot_reader {
      public:
         class section_reader {
            public:
               template<typename T>
               auto read_row( T& out ) -> std::enable_if_t<std::is_same<std::decay_t<T>, typename detail::snapshot_row_traits<T>::snapshot_type>::value,bool> {
                  auto reader = detail::make_row_reader(out);
                  return _reader.read_row(reader);
               }

               template<typename T>
               auto read_row( T& out, chainbase::database& ) -> std::enable_if_t<std::is_same<std::decay_t<T>, typename detail::snapshot_row_traits<T>::snapshot_type>::value,bool> {
                  return read_row(out);
               }

               template<typename T>
               auto read_row( T& out, chainbase::database& db ) -> std::enable_if_t<!std::is_same<std::decay_t<T>, typename detail::snapshot_row_traits<T>::snapshot_type>::value,bool> {
                  auto temp = typename detail::snapshot_row_traits<T>::snapshot_type();
                  auto reader = detail::make_row_reader(temp);
                  bool result = _reader.read_row(reader);
                  detail::snapshot_row_traits<T>::from_snapshot_row(std::move(temp), out, db);
                  return result;
               }

               bool empty() {
                  return _reader.empty();
               }

            private:
               friend class snapshot_reader;
               section_reader(snapshot_reader& _reader)
               :_reader(_reader)
               {}

               snapshot_reader& _reader;

         };

      template<typename F>
      void read_section(const std::string& section_name, F f) {
         set_section(section_name);
         auto section = section_reader(*this);
         f(section);
         clear_section();
      }

      template<typename T, typename F>
      void read_section(F f) {
         read_section(detail::snapshot_section_traits<T>::section_name(), f);
      }

      virtual void validate() = 0;

      virtual void return_to_header() = 0;

      virtual size_t total_row_count() = 0;

      virtual bool supports_threading() const {return false;}

      virtual bool has_section( const std::string& section_name ) const { return false; }

      virtual ~snapshot_reader(){};

      protected:
         virtual void set_section( const std::string& section_name ) = 0;
         virtual bool read_row( detail::abstract_snapshot_row_reader& row_reader ) = 0;
         virtual bool empty( ) = 0;
         virtual void clear_section() = 0;
   };

   using snapshot_reader_ptr = std::shared_ptr<snapshot_reader>;

   class variant_snapshot_writer : public snapshot_writer {
      public:
         variant_snapshot_writer(fc::mutable_variant_object& snapshot);

         const char* name() const override { return "variant snapshot"; }
         void write_start_section( const std::string& section_name ) override;
         void write_row( const detail::abstract_snapshot_row_writer& row_writer ) override;
         void write_end_section( ) override;
         void finalize();

      private:
         fc::mutable_variant_object& snapshot;
         std::string current_section_name;
         fc::variants current_rows;
   };

   class variant_snapshot_reader : public snapshot_reader {
      public:
         explicit variant_snapshot_reader(const fc::variant& snapshot);

         void validate() override;
         void set_section( const string& section_name ) override;
         bool read_row( detail::abstract_snapshot_row_reader& row_reader ) override;
         bool empty ( ) override;
         void clear_section() override;
         void return_to_header() override;
         size_t total_row_count() override;
         bool has_section( const std::string& section_name ) const override;

      private:
         const fc::variant& snapshot;
         const fc::variant_object* cur_section;
         uint64_t cur_row;
   };

   /**
    * Snapshot writer for v1 format.
    *
    * Writes sections sequentially to the output file. Each section's BLAKE3
    * hash is computed inline during writes via a buffering streambuf, so no
    * post-write re-read pass is needed. finalize() computes the root hash
    * and appends the section index and footer.
    */
   class threaded_snapshot_writer : public snapshot_writer {
      public:
         explicit threaded_snapshot_writer(std::filesystem::path snapshot_path);

         const char* name() const override { return "snapshot"; }

         /// Compute root hash, write index and footer.
         /// Must be called after all write_section() calls complete.
         void finalize();

         /// Returns the root hash (valid only after finalize()).
         fc::crypto::blake3 get_root_hash() const { return root_hash_; }

      protected:
         void write_start_section( const std::string& section_name ) override;
         void write_row( const detail::abstract_snapshot_row_writer& row_writer ) override;
         void write_end_section() override;

      private:
         struct section_info {
            std::string  name;
            uint64_t     data_offset = 0;
            uint64_t     data_size = 0;
            uint64_t     row_count = 0;
            fc::crypto::blake3 hash;
         };

         /// Buffering streambuf that incrementally BLAKE3-hashes all writes
         /// before forwarding them to the underlying file streambuf.
         class hashing_streambuf : public std::streambuf {
         public:
            hashing_streambuf() = default;
            void init(std::streambuf* sink, size_t buf_size = 1024 * 1024);

            /// Flush buffer, return accumulated section hash, and reset hasher.
            fc::crypto::blake3 finalize_section();

            /// Flush buffer and reset hasher (discards accumulated hash).
            void reset_section();

         protected:
            int_type overflow(int_type ch) override;
            int sync() override;
            std::streamsize xsputn(const char_type* s, std::streamsize count) override;
            pos_type seekoff(off_type, std::ios_base::seekdir,
                             std::ios_base::openmode = std::ios_base::out) override;

         private:
            bool flush_buffer();
            std::streambuf*    sink_ = nullptr;
            blake3_encoder     hasher_;
            std::vector<char>  buf_;
         };

         std::filesystem::path          snapshot_path_;
         std::ofstream                  out_;
         hashing_streambuf              hash_sbuf_;
         std::ostream                   hash_os_{&hash_sbuf_};
         detail::ostream_wrapper        wrapper_{hash_os_};

         std::string                    current_section_name_;
         uint64_t                       current_section_offset_ = 0;
         uint64_t                       current_row_count_ = 0;

         std::vector<section_info>      sections_;
         fc::crypto::blake3             root_hash_;
   };

   class ostream_json_snapshot_writer : public snapshot_writer {
      public:
         explicit ostream_json_snapshot_writer(std::ostream& snapshot);

         const char* name() const override { return "JSON snapshot"; }
         void write_start_section( const std::string& section_name ) override;
         void write_row( const detail::abstract_snapshot_row_writer& row_writer ) override;
         void write_end_section() override;
         void finalize();

      private:
         detail::ostream_wrapper snapshot;
         uint64_t                row_count;
   };

   class istream_json_snapshot_reader : public snapshot_reader {
      public:
         explicit istream_json_snapshot_reader(const std::filesystem::path& p);
         ~istream_json_snapshot_reader();

         void validate() override;
         void set_section( const string& section_name ) override;
         bool read_row( detail::abstract_snapshot_row_reader& row_reader ) override;
         bool empty ( ) override;
         void clear_section() override;
         void return_to_header() override;
         size_t total_row_count() override;

      private:
         bool validate_section() const;

         std::unique_ptr<struct istream_json_snapshot_reader_impl> impl;
   };

   /**
    * Memory-mapped snapshot reader for v1 format.
    *
    * Parses the section index on validate() for O(1) section lookup.
    * Thread-local datastream state allows multiple threads to read different
    * sections concurrently from the same memory-mapped file.
    */
   class threaded_snapshot_reader : public snapshot_reader {
      public:
         explicit threaded_snapshot_reader(const std::filesystem::path& snapshot_path);

         void validate() override;
         void set_section( const string& section_name ) override;
         bool read_row( detail::abstract_snapshot_row_reader& row_reader ) override;
         bool empty ( ) override;
         void clear_section() override;
         void return_to_header() override;
         size_t total_row_count() override;
         bool supports_threading() const override {return true;}

         bool has_section( const std::string& section_name ) const override;

         /// Returns the root hash read from the file header (valid after validate()).
         fc::crypto::blake3 get_root_hash() const { return root_hash_; }

      private:
         struct section_entry {
            std::string        name;
            uint64_t           data_offset = 0;
            uint64_t           data_size = 0;
            uint64_t           row_count = 0;
            fc::crypto::blake3 hash;
         };

         fc::random_access_file                   snapshot_file;
         const boost::interprocess::mapped_region mapped_snap;
         const char* const                        mapped_snap_addr;

         std::vector<section_entry>               section_index_;
         fc::crypto::blake3                       root_hash_;
         bool                                     validated_ = false;

         thread_local inline static fc::datastream<const char*> ds = fc::datastream<const char*>(nullptr, 0);
         thread_local inline static uint64_t                    num_rows;
         thread_local inline static uint64_t                    cur_row;
   };

   class integrity_hash_snapshot_writer : public snapshot_writer {
      public:
         integrity_hash_snapshot_writer() = default;

         const char* name() const override { return "integrity hash"; }
         void write_start_section( const std::string& section_name ) override;
         void write_row( const detail::abstract_snapshot_row_writer& row_writer ) override;
         void write_end_section( ) override;
         void finalize();

         fc::crypto::blake3 get_integrity_hash() const { return root_hash_; }

      private:
         struct section_hash {
            std::string        name;
            fc::crypto::blake3 hash;
         };

         blake3_encoder                  current_encoder_;
         std::string                     current_section_name_;
         std::vector<section_hash>       section_hashes_;
         fc::crypto::blake3              root_hash_;
   };

   struct snapshot_written_row_counter {
      snapshot_written_row_counter(const size_t total, const char* name) : total(total), name(name) {}
      void progress() {
         auto c = count.fetch_add(1, std::memory_order_relaxed) + 1;
         if(c % 50000 == 0) {
            auto now = time(NULL);
            auto expected = last_print.load(std::memory_order_relaxed);
            if(now - expected >= 5 && last_print.compare_exchange_strong(expected, now, std::memory_order_relaxed)) {
               ilog("{} creation {}% complete", name, std::min((unsigned)(((double)c/total)*100),100u));
            }
         }
      }
      std::atomic<size_t> count{0};
      const size_t total = 0;
      const char* name = nullptr;
      std::atomic<time_t> last_print{time(NULL)};
   };

   fc::variant snapshot_info(snapshot_reader& snapshot);
}}
