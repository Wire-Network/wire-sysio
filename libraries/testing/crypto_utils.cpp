#include <format>
#include <memory>
#include <string>
#include <vector>
#include <type_traits>

#include <boost/dll.hpp>
#include <boost/process.hpp>
#include <boost/process/io.hpp>
#include <boost/process/spawn.hpp>
#include <fc/io/fstream.hpp>
#include <fc/io/json.hpp>
#include <sysio/testing/build_info.hpp>
#include <sysio/testing/crypto_utils.hpp>

namespace sysio::testing {
namespace bfs = boost::filesystem;
namespace bp = boost::process;

keygen_result generate_external_test_key_and_sig(const std::string& keygen_script) {
   FC_ASSERT(is_keygen_script(keygen_script));

   auto tools_path  = get_build_root_path() / "tools";
   auto script_path = tools_path / keygen_script;

   std::vector<std::string> proc_args;
   std::string output;
   auto        python_exe = boost::process::search_path("python");
   FC_ASSERT(!python_exe.empty());
   FC_ASSERT(boost::filesystem::exists(script_path));

   bp::ipstream pipe_stream; // pipe for child's stdout

   bp::child keygen_proc(python_exe, script_path.string(),
                         // proc_args,
                         bp::std_in.close(), bp::std_out > pipe_stream, bp::std_err > bp::null);
   std::string line;
   while (std::getline(pipe_stream, line))
      output += line + "\n";

   keygen_proc.wait();
   FC_ASSERT(keygen_proc.exit_code() == 0);

   keygen_result res;
   auto          res_obj = fc::json::from_string(
         output,
         fc::json::parse_type::relaxed_parser
         )
      .as<fc::variant_object>();
   from_variant(res_obj, res);

   return res;
}


std::string to_private_key_spec(const std::string& priv) {
   return std::format("KEY:{}", priv);
}

// ethereum examples:
// ethereum-key-01,ethereum,ethereum,0xfc5422471c9e31a6cd6632a2858eeaab39f9a7eec5f48eedecf53b8398521af1c86c9fce17312900cbb11e2e2ec1fb706598065f855c2f8f2067e1fbc1ba54c8,KEY:0x8f2cdaeb8e036865421c79d4cc42c7704af5cef0f592b2e5c993e2ba7d328248
// ethereum-key-02,ethereum,ethereum,0x3a0d6f5e4e7f3a8ce6d5f5c1f3e6e8b9c1d2e3f4a5b6c7d8e9f0a1b2c3d4e5f6a7b8c9d0e1f2030405060708090a0b0c0d,KEY:0x4c0883a69102937d6231471b5dbb6204fe5129617082792ae468d01a3f362318
std::string to_provider_spec(const std::string&           key_name, fc::crypto::chain_kind_t target_chain,
                             fc::crypto::chain_key_type_t key_type, std::string              public_key_text,
                             std::string                  private_key_provider_spec) {
   using namespace fc::crypto;
   return std::format("{},{},{},{},{}", key_name, chain_kind_reflector::to_fc_string(target_chain),
                      chain_key_type_reflector::to_fc_string(key_type), public_key_text, private_key_provider_spec);
}

keygen_result load_keygen_fixture(const std::string& keygen_name, std::uint32_t id) {
   FC_ASSERT(is_keygen_name(keygen_name));
   auto json_filename = std::format("{}-keygen-{:02}.json", keygen_name, id);
   auto json_path     = get_test_fixtures_path() / json_filename;

   FC_ASSERT(boost::filesystem::exists(json_path));

   std::string json_data;
   fc::read_file_contents(json_path.string(), json_data);

   keygen_result res;
   auto res_obj = fc::json::from_string(json_data, fc::json::parse_type::relaxed_parser).as<fc::variant_object>();

   from_variant(res_obj, res);

   return res;
}

std::string keygen_fixture_to_spec(const std::string& keygen_name, std::uint32_t id) {
   auto fixture          = load_keygen_fixture(keygen_name, id);
   auto private_key_spec = to_private_key_spec(fixture.private_key);
   auto spec             = to_provider_spec(
      fixture.key_name,
      fixture.chain_type,
      fixture.chain_key_type,
      fixture.public_key,
      private_key_spec
      );
   return spec;
}

std::vector<std::string> keygen_fixtures_to_specs(const std::string& keygen_name) {
   std::vector<std::string> specs;
   for (std::uint32_t id = 1;; ++id) {
      auto json_filename = std::format("{}-keygen-{:02}.json", keygen_name, id);
      auto json_path     = get_test_fixtures_path() / json_filename;
      if (!boost::filesystem::exists(json_path))
         break;
      specs.push_back(keygen_fixture_to_spec(keygen_name, id));
   }
   return specs;
}


}