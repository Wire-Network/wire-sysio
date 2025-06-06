#pragma once

#include <fc/variant_object.hpp>
#include <snapshots.hpp>

using namespace sysio::chain;
using namespace sysio::testing;

struct variant_snapshot_suite {
   using writer_t = variant_snapshot_writer;
   using reader_t = variant_snapshot_reader;
   using write_storage_t = fc::mutable_variant_object;
   using snapshot_t = fc::variant;

   struct writer : public writer_t {
      writer( const std::shared_ptr<write_storage_t>& storage )
      :writer_t(*storage)
      ,storage(storage)
      {

      }

      std::shared_ptr<write_storage_t> storage;
   };

   struct reader : public reader_t {
      explicit reader(const snapshot_t& storage)
      :reader_t(storage)
      {}
   };


   static auto get_writer() {
      return std::make_shared<writer>(std::make_shared<write_storage_t>());
   }

   static auto finalize(const std::shared_ptr<writer>& w) {
      w->finalize();
      return snapshot_t(*w->storage);
   }

   static auto get_reader( const snapshot_t& buffer) {
      return std::make_shared<reader>(buffer);
   }

   static snapshot_t load_from_file(const std::string& filename) {
      snapshot_input_file<snapshot::json> file(filename);
      return file.read();
   }

   static void write_to_file( const std::string& basename, const snapshot_t& snapshot ) {
     snapshot_output_file<snapshot::json> file(basename);
     file.write<snapshot_t>(snapshot);
   }
};

struct buffered_snapshot_suite {
   using writer_t = ostream_snapshot_writer;
   using reader_t = istream_snapshot_reader;
   using write_storage_t = std::ostringstream;
   using snapshot_t = std::string;
   using read_storage_t = std::istringstream;

   struct writer : public writer_t {
      writer( const std::shared_ptr<write_storage_t>& storage )
      :writer_t(*storage)
      ,storage(storage)
      {

      }

      std::shared_ptr<write_storage_t> storage;
   };

   struct reader : public reader_t {
      explicit reader(const std::shared_ptr<read_storage_t>& storage)
      :reader_t(*storage)
      ,storage(storage)
      {}

      std::shared_ptr<read_storage_t> storage;
   };


   static auto get_writer() {
      return std::make_shared<writer>(std::make_shared<write_storage_t>());
   }

   static auto finalize(const std::shared_ptr<writer>& w) {
      w->finalize();
      return w->storage->str();
   }

   static auto get_reader( const snapshot_t& buffer) {
      return std::make_shared<reader>(std::make_shared<read_storage_t>(buffer));
   }

   static snapshot_t load_from_file(const std::string& filename) {
      snapshot_input_file<snapshot::binary> file(filename);
      return file.read_as_string();
   }

   static void write_to_file( const std::string& basename, const snapshot_t& snapshot ) {
      snapshot_output_file<snapshot::binary> file(basename);
      file.write<snapshot_t>(snapshot);
   }
};


struct json_snapshot_suite {
   using writer_t = ostream_json_snapshot_writer;
   using reader_t = istream_json_snapshot_reader;
   using write_storage_t = std::ostringstream;
   using snapshot_t = std::string;
   using read_storage_t = std::istringstream;

   struct writer : public writer_t {
      writer( const std::shared_ptr<write_storage_t>& storage )
      :writer_t(*storage)
      ,storage(storage)
      {

      }

      std::shared_ptr<write_storage_t> storage;
   };

   static std::string temp_file() {
      static fc::temp_directory temp_dir;
      auto temp_file = temp_dir.path() / "temp.bin.json";
      return temp_file.string();
   }

   struct reader : public reader_t {
      explicit reader(const std::filesystem::path& p)
      :reader_t(p)
      {}
      ~reader() {
         remove(json_snapshot_suite::temp_file().c_str());
      }
   };


   static auto get_writer() {
      return std::make_shared<writer>(std::make_shared<write_storage_t>());
   }

   static auto finalize(const std::shared_ptr<writer>& w) {
      w->finalize();
      return w->storage->str();
   }

   static auto get_reader( const snapshot_t& buffer) {
      std::ofstream fs(json_snapshot_suite::temp_file());
      fs << buffer;
      fs.close();
      std::filesystem::path p(json_snapshot_suite::temp_file());
      return std::make_shared<reader>(p);
   }

   static snapshot_t load_from_file(const std::string& filename) {
      snapshot_input_file<snapshot::json_snapshot> file(filename);
      return file.read_as_string();
   }

   static void write_to_file( const std::string& basename, const snapshot_t& snapshot ) {
      snapshot_output_file<snapshot::json_snapshot> file(basename);
      file.write<snapshot_t>(snapshot);
   }
};

using snapshot_suites = boost::mpl::list<variant_snapshot_suite, buffered_snapshot_suite, json_snapshot_suite>;
