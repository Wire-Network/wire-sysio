#define RAPIDJSON_NAMESPACE sysio_rapidjson // This is ABSOLUTELY necessary anywhere that is using sysio_rapidjson

#include <sysio/chain/block_timestamp.hpp>
#include <sysio/chain/chain_snapshot.hpp>
#include <sysio/chain/exceptions.hpp>
#include <sysio/chain/genesis_state.hpp>
#include <sysio/chain/global_property_object.hpp>
#include <sysio/chain/snapshot.hpp>
#include <sysio/chain/snapshot_detail.hpp>

#include <fc/scoped_exit.hpp>
#include <fc/variant_object.hpp>
#include <fc/io/json.hpp>

#include <rapidjson/document.h>
#include <rapidjson/filereadstream.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <sysio/chain/blake3_encoder.hpp>

#include <algorithm>

using namespace sysio_rapidjson;

namespace sysio { namespace chain {

// ---- variant_snapshot_writer ----

variant_snapshot_writer::variant_snapshot_writer(fc::mutable_variant_object& snapshot)
: snapshot(snapshot)
{
   snapshot.set("sections", fc::variants());
   snapshot.set("version", current_snapshot_version );
}

void variant_snapshot_writer::write_start_section( const std::string& section_name ) {
   current_rows.clear();
   current_section_name = section_name;
}

void variant_snapshot_writer::write_row( const detail::abstract_snapshot_row_writer& row_writer ) {
   current_rows.emplace_back(row_writer.to_variant());
}

void variant_snapshot_writer::write_end_section( ) {
   snapshot["sections"].get_array().emplace_back(fc::mutable_variant_object()("name", std::move(current_section_name))("rows", std::move(current_rows)));
}

void variant_snapshot_writer::finalize() {

}

// ---- variant_snapshot_reader ----

variant_snapshot_reader::variant_snapshot_reader(const fc::variant& snapshot)
:snapshot(snapshot)
,cur_section(nullptr)
,cur_row(0)
{
}

void variant_snapshot_reader::validate() {
   SYS_ASSERT(snapshot.is_object(), snapshot_validation_exception,
         "Variant snapshot is not an object");
   const fc::variant_object& o = snapshot.get_object();

   SYS_ASSERT(o.contains("version"), snapshot_validation_exception,
         "Variant snapshot has no version");

   const auto& version = o["version"];
   SYS_ASSERT(version.is_integer(), snapshot_validation_exception,
         "Variant snapshot version is not an integer");

   SYS_ASSERT(version.as_uint64() == (uint64_t)current_snapshot_version, snapshot_validation_exception,
         "Variant snapshot is an unsupported version.  Expected : {}, Got: {}",
         current_snapshot_version, o["version"].as_uint64());

   SYS_ASSERT(o.contains("sections"), snapshot_validation_exception,
         "Variant snapshot has no sections");

   const auto& sections = o["sections"];
   SYS_ASSERT(sections.is_array(), snapshot_validation_exception, "Variant snapshot sections is not an array");

   const auto& section_array = sections.get_array();
   for( const auto& section: section_array ) {
      SYS_ASSERT(section.is_object(), snapshot_validation_exception, "Variant snapshot section is not an object");

      const auto& so = section.get_object();
      SYS_ASSERT(so.contains("name"), snapshot_validation_exception,
            "Variant snapshot section has no name");

      SYS_ASSERT(so["name"].is_string(), snapshot_validation_exception,
                 "Variant snapshot section name is not a string");

      SYS_ASSERT(so.contains("rows"), snapshot_validation_exception,
                 "Variant snapshot section has no rows");

      SYS_ASSERT(so["rows"].is_array(), snapshot_validation_exception,
                 "Variant snapshot section rows is not an array");
   }
}

bool variant_snapshot_reader::has_section( const std::string& section_name ) const {
   const auto& sections = snapshot["sections"].get_array();
   for( const auto& section: sections ) {
      if (section["name"].as_string() == section_name) {
         return true;
      }
   }
   return false;
}

void variant_snapshot_reader::set_section( const string& section_name ) {
   const auto& sections = snapshot["sections"].get_array();
   for( const auto& section: sections ) {
      if (section["name"].as_string() == section_name) {
         cur_section = &section.get_object();
         return;
      }
   }

   SYS_THROW(snapshot_exception, "Variant snapshot has no section named {}", section_name);
}

bool variant_snapshot_reader::read_row( detail::abstract_snapshot_row_reader& row_reader ) {
   const auto& rows = (*cur_section)["rows"].get_array();
   row_reader.provide(rows.at(cur_row++));
   return cur_row < rows.size();
}

bool variant_snapshot_reader::empty ( ) {
   const auto& rows = (*cur_section)["rows"].get_array();
   return rows.empty();
}

void variant_snapshot_reader::clear_section() {
   cur_section = nullptr;
   cur_row = 0;
}

void variant_snapshot_reader::return_to_header() {
   clear_section();
}

size_t variant_snapshot_reader::total_row_count() {
   size_t total = 0;

   const fc::variants& sections = snapshot["sections"].get_array();
   for(const fc::variant& section : sections)
      total += section["rows"].get_array().size();

   return total;
}

// ---- threaded_snapshot_writer::hashing_streambuf ----

void threaded_snapshot_writer::hashing_streambuf::init(std::streambuf* sink, size_t buf_size) {
   sink_ = sink;
   buf_.resize(buf_size);
   setp(buf_.data(), buf_.data() + buf_.size());
}

bool threaded_snapshot_writer::hashing_streambuf::flush_buffer() {
   const auto n = pptr() - pbase();
   if(n > 0) {
      hasher_.write(pbase(), n);
      if(sink_->sputn(pbase(), n) != n)
         return false;
      setp(buf_.data(), buf_.data() + buf_.size());
   }
   return true;
}

threaded_snapshot_writer::hashing_streambuf::int_type
threaded_snapshot_writer::hashing_streambuf::overflow(int_type ch) {
   if(!sink_ || !flush_buffer())
      return traits_type::eof();
   if(!traits_type::eq_int_type(ch, traits_type::eof())) {
      *pbase() = traits_type::to_char_type(ch);
      pbump(1);
   }
   return ch;
}

int threaded_snapshot_writer::hashing_streambuf::sync() {
   return flush_buffer() ? 0 : -1;
}

std::streamsize threaded_snapshot_writer::hashing_streambuf::xsputn(
      const char_type* s, std::streamsize count) {
   std::streamsize remaining = count;
   while(remaining > 0) {
      std::streamsize space = epptr() - pptr();
      if(space == 0) {
         if(!flush_buffer())
            return count - remaining;
         space = epptr() - pptr();
      }
      std::streamsize chunk = std::min(space, remaining);
      std::memcpy(pptr(), s, chunk);
      pbump(static_cast<int>(chunk));
      s += chunk;
      remaining -= chunk;
   }
   return count;
}

threaded_snapshot_writer::hashing_streambuf::pos_type
threaded_snapshot_writer::hashing_streambuf::seekoff(
      off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which) {
   if(!flush_buffer())
      return pos_type(off_type(-1));
   return sink_->pubseekoff(off, dir, which);
}

fc::crypto::blake3 threaded_snapshot_writer::hashing_streambuf::finalize_section() {
   flush_buffer();
   auto h = hasher_.result();
   hasher_.reset();
   return h;
}

void threaded_snapshot_writer::hashing_streambuf::reset_section() {
   flush_buffer();
   hasher_.reset();
}

// ---- threaded_snapshot_writer (v1 binary format) ----

threaded_snapshot_writer::threaded_snapshot_writer(std::filesystem::path snapshot_path)
: snapshot_path_(std::move(snapshot_path))
, out_(snapshot_path_, std::ios::binary)
{
   SYS_ASSERT(out_.good(), snapshot_exception, "Failed to open snapshot output file: {}", snapshot_path_.string());

   hash_sbuf_.init(out_.rdbuf());

   // Write header: magic(4) + version(4) = 8 bytes
   // Written through hash_os_; hash is discarded on first reset_section() call.
   const auto totem = magic_number;
   hash_os_.write(reinterpret_cast<const char*>(&totem), sizeof(totem));
   uint32_t version = current_snapshot_version;
   hash_os_.write(reinterpret_cast<const char*>(&version), sizeof(version));
}

void threaded_snapshot_writer::write_start_section(const std::string& section_name) {
   current_section_name_ = section_name;
   hash_sbuf_.reset_section();
   current_section_offset_ = static_cast<uint64_t>(hash_os_.tellp());
   current_row_count_ = 0;
}

void threaded_snapshot_writer::write_row(const detail::abstract_snapshot_row_writer& row_writer) {
   row_writer.write(wrapper_);
   ++current_row_count_;
}

void threaded_snapshot_writer::write_end_section() {
   section_info info;
   info.name = std::move(current_section_name_);
   info.data_offset = current_section_offset_;
   info.data_size = static_cast<uint64_t>(hash_os_.tellp()) - current_section_offset_;
   info.row_count = current_row_count_;
   info.hash = hash_sbuf_.finalize_section();
   sections_.push_back(std::move(info));
}

void threaded_snapshot_writer::finalize() {
   hash_os_.flush();
   SYS_ASSERT(hash_os_.good(), snapshot_exception,
              "Failed to write snapshot section data: {}", snapshot_path_.string());

   const uint64_t index_offset = static_cast<uint64_t>(hash_os_.tellp());

   // Sort sections by name for deterministic order
   std::sort(sections_.begin(), sections_.end(),
             [](const section_info& a, const section_info& b) { return a.name < b.name; });

   // Compute root hash = BLAKE3(hash_0 || hash_1 || ... || hash_n)
   // Section hashes were computed inline during writes.
   {
      blake3_encoder root_hasher;
      for(const auto& s : sections_) {
         root_hasher.write(s.hash.char_data(), s.hash.data_size());
      }
      root_hash_ = root_hasher.result();
   }

   const uint32_t num_sections = static_cast<uint32_t>(sections_.size());

   // Write section index directly to file (not hashed)
   for(const auto& s : sections_) {
      out_.write(s.name.data(), s.name.size());
      out_.put(0);
      out_.write(reinterpret_cast<const char*>(&s.data_offset), sizeof(s.data_offset));
      out_.write(reinterpret_cast<const char*>(&s.data_size), sizeof(s.data_size));
      out_.write(reinterpret_cast<const char*>(&s.row_count), sizeof(s.row_count));
      out_.write(s.hash.char_data(), s.hash.data_size());
   }

   // Write footer: num_sections + root_hash + index_offset
   out_.write(reinterpret_cast<const char*>(&num_sections), sizeof(num_sections));
   out_.write(root_hash_.char_data(), root_hash_.data_size());
   out_.write(reinterpret_cast<const char*>(&index_offset), sizeof(index_offset));

   out_.flush();
   SYS_ASSERT(out_.good(), snapshot_exception, "Failed to write snapshot file: {}", snapshot_path_.string());
}

// ---- ostream_json_snapshot_writer ----

ostream_json_snapshot_writer::ostream_json_snapshot_writer(std::ostream& snapshot)
      :snapshot(snapshot)
      ,row_count(0)
{
   snapshot << "{\n";
   // write magic number
   auto totem = magic_number;
   snapshot << "\"magic_number\":" << fc::json::to_string(totem, fc::time_point::maximum()) << "\n";

   // write version
   auto version = current_snapshot_version;
   snapshot << ",\"version\":" << fc::json::to_string(version, fc::time_point::maximum()) << "\n";
}

void ostream_json_snapshot_writer::write_start_section( const std::string& section_name )
{
   row_count = 0;
   snapshot.inner << "," << fc::json::to_string(section_name, fc::time_point::maximum()) << ":{\n\"rows\":[\n";
}

void ostream_json_snapshot_writer::write_row( const detail::abstract_snapshot_row_writer& row_writer ) {
   const auto yield = [&](size_t s) {};

   if(row_count != 0) snapshot.inner << ",";
   snapshot.inner << fc::json::to_string(row_writer.to_variant(), yield) << "\n";
   ++row_count;
}

void ostream_json_snapshot_writer::write_end_section( ) {
   snapshot.inner << "],\n\"num_rows\":" << row_count << "\n}\n";
   row_count = 0;
}

void ostream_json_snapshot_writer::finalize() {
   snapshot.inner << "}\n";
   snapshot.inner.flush();
}

// ---- istream_json_snapshot_reader ----

struct istream_json_snapshot_reader_impl {
   uint64_t num_rows;
   uint64_t cur_row;
   sysio_rapidjson::Document doc;
   std::string sec_name;
};

istream_json_snapshot_reader::~istream_json_snapshot_reader() = default;

istream_json_snapshot_reader::istream_json_snapshot_reader(const std::filesystem::path& p)
   : impl{new istream_json_snapshot_reader_impl{0, 0, {}, {}}}
{
   FILE* fp = fopen(p.string().c_str(), "rb");
   SYS_ASSERT(fp, snapshot_exception, "Failed to open JSON snapshot: {}", p.generic_string());
   auto close = fc::make_scoped_exit( [&fp]() { fclose( fp ); } );
   char readBuffer[65536];
   sysio_rapidjson::FileReadStream is(fp, readBuffer, sizeof(readBuffer));
   impl->doc.ParseStream(is);
}

void istream_json_snapshot_reader::validate() {
   try {
      // validate totem
      auto expected_totem = ostream_json_snapshot_writer::magic_number;
      SYS_ASSERT(impl->doc.HasMember("magic_number"), snapshot_exception, "magic_number section not found" );
      auto actual_totem = impl->doc["magic_number"].GetUint();
      SYS_ASSERT( actual_totem == expected_totem, snapshot_exception, "JSON snapshot has unexpected magic number" );

      // validate version
      auto expected_version = current_snapshot_version;
      SYS_ASSERT(impl->doc.HasMember("version"), snapshot_exception, "version section not found" );
      auto actual_version = impl->doc["version"].GetUint();
      SYS_ASSERT( actual_version == expected_version, snapshot_exception,
                  "JSON snapshot is an unsupported version.  Expected : {}, Got: {}",
                  expected_version, actual_version );

   } catch( const std::exception& e ) {  \
      snapshot_exception fce(FC_LOG_MESSAGE( warn, "JSON snapshot validation threw IO exception ({})", e.what()));
      throw fce;
   }
}

bool istream_json_snapshot_reader::validate_section() const {
   return true;
}

void istream_json_snapshot_reader::set_section( const string& section_name ) {
   SYS_ASSERT( impl->doc.HasMember( section_name.c_str() ), snapshot_exception, "JSON snapshot has no section {}", section_name );
   SYS_ASSERT( impl->doc[section_name.c_str()].HasMember( "num_rows" ), snapshot_exception, "JSON snapshot {} num_rows not found", section_name );
   SYS_ASSERT( impl->doc[section_name.c_str()].HasMember( "rows" ), snapshot_exception, "JSON snapshot {} rows not found", section_name );
   SYS_ASSERT( impl->doc[section_name.c_str()]["rows"].IsArray(), snapshot_exception, "JSON snapshot {} rows is not an array", section_name );

   impl->sec_name = section_name;
   impl->num_rows = impl->doc[section_name.c_str()]["num_rows"].GetInt();
   ilog( "reading {}, num_rows: {}", section_name, impl->num_rows );
}

bool istream_json_snapshot_reader::read_row( detail::abstract_snapshot_row_reader& row_reader ) {
   SYS_ASSERT( impl->cur_row < impl->num_rows, snapshot_exception, "JSON snapshot {}'s cur_row {} >= num_rows {}",
               impl->sec_name, impl->cur_row, impl->num_rows );

   const sysio_rapidjson::Value& rows = impl->doc[impl->sec_name.c_str()]["rows"];
   sysio_rapidjson::StringBuffer buffer;
   sysio_rapidjson::Writer<sysio_rapidjson::StringBuffer> writer( buffer );
   rows[impl->cur_row].Accept( writer );

   const auto& row = fc::json::from_string( buffer.GetString() );
   row_reader.provide( row );
   return ++impl->cur_row < impl->num_rows;
}

bool istream_json_snapshot_reader::empty ( ) {
   return impl->num_rows == 0;
}

void istream_json_snapshot_reader::clear_section() {
   impl->num_rows = 0;
   impl->cur_row = 0;
   impl->sec_name = "";
}

void istream_json_snapshot_reader::return_to_header() {
   clear_section();
}

size_t istream_json_snapshot_reader::total_row_count() {
   size_t total = 0;

   for(const auto& section : impl->doc.GetObject())
      if(section.value.IsObject() && section.value.HasMember("num_rows"))
        total += section.value["num_rows"].GetUint64();

   return total;
}

// ---- threaded_snapshot_reader (v1 binary format) ----

threaded_snapshot_reader::threaded_snapshot_reader(const std::filesystem::path& snapshot_path) :
  snapshot_file(snapshot_path, fc::random_access_file::read_only),
  mapped_snap(snapshot_file, boost::interprocess::read_only),
  mapped_snap_addr((char*)mapped_snap.get_address())
{
   validate();
}

void threaded_snapshot_reader::load_index() {
   if(index_loaded_)
      return;
   try {
      const uint64_t file_size = mapped_snap.get_size();
      // header = magic(uint32) + version(uint32), footer = num_sections(uint32) + root_hash(blake3) + index_offset(uint64)
      constexpr uint64_t header_size = sizeof(uint32_t) + sizeof(uint32_t);
      constexpr uint64_t footer_size = sizeof(uint32_t) + fc::crypto::blake3::byte_size + sizeof(uint64_t);
      constexpr uint64_t min_file_size = header_size + footer_size;

      SYS_ASSERT(file_size >= min_file_size, snapshot_exception, "Snapshot file too small");

      // Read header
      fc::datastream<const char*> hds(mapped_snap_addr, file_size);

      uint32_t actual_magic;
      hds.read(reinterpret_cast<char*>(&actual_magic), sizeof(actual_magic));
      SYS_ASSERT(actual_magic == snapshot_writer::magic_number, snapshot_exception,
                 "Binary snapshot has unexpected magic number!");

      uint32_t actual_version;
      hds.read(reinterpret_cast<char*>(&actual_version), sizeof(actual_version));
      SYS_ASSERT(actual_version == current_snapshot_version, snapshot_exception,
                 "Binary snapshot is an unsupported version. Expected: {}, Got: {}",
                 current_snapshot_version, actual_version);

      // Read footer from end of file
      const char* footer_start = mapped_snap_addr + file_size - footer_size;
      fc::datastream<const char*> fds(footer_start, footer_size);

      uint32_t num_sections;
      fds.read(reinterpret_cast<char*>(&num_sections), sizeof(num_sections));
      fds.read(root_hash_.char_data(), root_hash_.data_size());
      uint64_t index_offset;
      fds.read(reinterpret_cast<char*>(&index_offset), sizeof(index_offset));

      SYS_ASSERT(index_offset < file_size - footer_size, snapshot_exception,
                 "Section index offset beyond file bounds");

      // Parse section index at index_offset
      fc::datastream<const char*> ids(mapped_snap_addr + index_offset, file_size - footer_size - index_offset);

      section_index_.clear();
      section_index_.reserve(num_sections);

      for(uint32_t i = 0; i < num_sections; i++) {
         section_entry entry;

         // Read null-terminated section name
         const char* name_start = ids.pos();
         const char* name_end = name_start + ids.remaining();
         const char* p = name_start;
         while(p < name_end && *p != '\0')
            ++p;
         SYS_ASSERT(p < name_end, snapshot_exception, "Section name not null-terminated");
         entry.name.assign(name_start, p - name_start);
         ids.skip(entry.name.size() + 1);

         ids.read(reinterpret_cast<char*>(&entry.data_offset), sizeof(entry.data_offset));
         ids.read(reinterpret_cast<char*>(&entry.data_size), sizeof(entry.data_size));
         ids.read(reinterpret_cast<char*>(&entry.row_count), sizeof(entry.row_count));
         ids.read(entry.hash.char_data(), entry.hash.data_size());

         SYS_ASSERT(entry.data_offset + entry.data_size <= file_size, snapshot_exception,
                    "Section '{}' data extends beyond end of file", entry.name);

         section_index_.push_back(std::move(entry));
      }

      index_loaded_ = true;
   } FC_LOG_AND_RETHROW()
}

void threaded_snapshot_reader::validate() {
   if(validated_)
      return;

   load_index();

   try {
      // Verify per-section hashes by re-hashing each section's data from mmap
      for(const auto& entry : section_index_) {
         auto computed = blake3_encoder::hash(mapped_snap_addr + entry.data_offset, entry.data_size);
         SYS_ASSERT(computed == entry.hash, snapshot_exception,
                    "Section '{}' hash mismatch. Snapshot file may be corrupted.", entry.name);
      }

      // Verify root hash = BLAKE3(section_hash_0 || section_hash_1 || ...)
      fc::crypto::blake3 computed_root;
      {
         blake3_encoder root_hasher;
         for(const auto& entry : section_index_) {
            root_hasher.write(entry.hash.char_data(), entry.hash.data_size());
         }
         computed_root = root_hasher.result();
      }
      SYS_ASSERT(computed_root == root_hash_, snapshot_exception,
                 "Snapshot root hash mismatch. File may be corrupted.");

      validated_ = true;
   } FC_LOG_AND_RETHROW()
}

bool threaded_snapshot_reader::has_section(const std::string& section_name) const {
   for(const auto& entry : section_index_) {
      if(entry.name == section_name) {
         return true;
      }
   }
   return false;
}

void threaded_snapshot_reader::set_section(const string& section_name) {
   SYS_ASSERT(index_loaded_, snapshot_exception, "Snapshot index must be loaded before reading sections");

   for(const auto& entry : section_index_) {
      if(entry.name == section_name) {
         cur_row = 0;
         num_rows = entry.row_count;
         ds = fc::datastream<const char*>(mapped_snap_addr + entry.data_offset, entry.data_size);
         return;
      }
   }

   SYS_THROW(snapshot_exception, "Binary snapshot has no section named {}",  section_name);
}

bool threaded_snapshot_reader::read_row(detail::abstract_snapshot_row_reader& row_reader) {
   row_reader.provide(ds);
   return ++cur_row < num_rows;
}

bool threaded_snapshot_reader::empty ( ) {
   return num_rows == 0;
}

void threaded_snapshot_reader::clear_section() {
#ifdef __linux__
   //this might work elsewhere, but unsure about alignment requirements on madvise() elsewhere
   if(num_rows) {
      uintptr_t endp = (uintptr_t)ds.pos();
      ds.seekp(0);
      uintptr_t p = (uintptr_t)ds.pos();
      madvise((char*)(p & ~(boost::interprocess::mapped_region::get_page_size()-1)), endp-p, MADV_DONTNEED);
   }
#endif
   num_rows = 0;
   cur_row = 0;
}

void threaded_snapshot_reader::return_to_header() {
   clear_section();
}

size_t threaded_snapshot_reader::total_row_count() {
   SYS_ASSERT(index_loaded_, snapshot_exception, "Snapshot index must be loaded before querying row count");

   size_t total = 0;
   for(const auto& entry : section_index_) {
      total += entry.row_count;
   }
   return total;
}

// ---- integrity_hash_snapshot_writer ----
// Produces the same root hash as threaded_snapshot_writer by hashing each
// section independently with BLAKE3, sorting by name, then combining.

void integrity_hash_snapshot_writer::write_start_section( const std::string& section_name ) {
   current_section_name_ = section_name;
   current_encoder_.reset();
}

void integrity_hash_snapshot_writer::write_row( const detail::abstract_snapshot_row_writer& row_writer ) {
   row_writer.write(current_encoder_);
}

void integrity_hash_snapshot_writer::write_end_section() {
   section_hashes_.push_back({std::move(current_section_name_), current_encoder_.result()});
}

void integrity_hash_snapshot_writer::finalize() {
   std::sort(section_hashes_.begin(), section_hashes_.end(),
             [](const section_hash& a, const section_hash& b) { return a.name < b.name; });

   blake3_encoder root_hasher;
   for(const auto& s : section_hashes_) {
      root_hasher.write(s.hash.char_data(), s.hash.data_size());
   }
   root_hash_ = root_hasher.result();
}

// ---- snapshot_info ----

fc::variant snapshot_info(snapshot_reader& snapshot) {
   chain_snapshot_header header;
   snapshot.read_section<chain_snapshot_header>([&](auto &section){
      section.read_row(header);
   });
   if(header.version < chain_snapshot_header::minimum_compatible_version || header.version > chain_snapshot_header::current_version)
      wlog("Snapshot version {} is not supported by this version of sys-util, trying to parse anyways...", header.version);

   chain_id_type chain_id = chain_id_type::empty_chain_id();
   if(header.version <= 1) {
      snapshot.read_section<global_property_object>([&]( auto &section ) {
         snapshot_global_property_object snap_global_properties;
         section.read_row(snap_global_properties);
         chain_id = snap_global_properties.chain_id;
      });
   }

   block_id_type head_block;
   block_timestamp_type head_block_time;
   if(header.version <= snapshot_detail::snapshot_block_state_data_v1::maximum_version) {
      snapshot.read_section("sysio::chain::block_state", [&]( auto &section ) {
         snapshot_detail::snapshot_block_state_data_v1 header_state;
         section.read_row(header_state);
         head_block = header_state.bs.block_id;
         head_block_time = header_state.bs.header.timestamp;
      });
   }

   return fc::mutable_variant_object()("version", header.version)
                                      ("chain_id", chain_id)
                                      ("head_block_id", head_block)
                                      ("head_block_num", block_header::num_from_id(head_block))
                                      ("head_block_time", head_block_time);
}

}}
