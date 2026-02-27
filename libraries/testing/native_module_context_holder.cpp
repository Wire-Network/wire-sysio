#include <sysio/testing/native_module_context_holder.hpp>

#ifdef SYSIO_NATIVE_MODULE_RUNTIME_ENABLED
#include <sysio/chain/webassembly/native-module/native-module.hpp>
#endif

#include <fc/crypto/sha256.hpp>
#include <fc/log/logger.hpp>

#include <fstream>
#include <map>

namespace sysio::testing {

void native_module_context_holder::register_native_so(const chain::digest_type& code_hash,
                                                      const std::filesystem::path& so_path)
{
#ifdef SYSIO_NATIVE_MODULE_RUNTIME_ENABLED
   auto link_path = chain::webassembly::native_module::native_runtime::code_dir() / (code_hash.str() + ".so");
   std::error_code ec;
   std::filesystem::remove(link_path, ec);
   std::filesystem::create_symlink(std::filesystem::absolute(so_path), link_path, ec);
   if (ec) {
      wlog("Failed to create native contract symlink {} -> {}: {}",
           link_path.string(), so_path.string(), ec.message());
   }
#endif
}

// Static registry: maps WASM code_hash -> native .so path.
// Populated at startup by register_native_contract().
static std::map<chain::digest_type, std::filesystem::path>& native_so_registry() {
   static std::map<chain::digest_type, std::filesystem::path> registry;
   return registry;
}

void native_module_context_holder::register_native_contract(const std::vector<uint8_t>& wasm,
                                                             const std::filesystem::path& so_path)
{
   if (!std::filesystem::exists(so_path)) return;
   auto code_hash = fc::sha256::hash(reinterpret_cast<const char*>(wasm.data()), wasm.size());
   native_so_registry()[code_hash] = so_path;
   // Also create the symlink immediately
   register_native_so(code_hash, so_path);
}

void native_module_context_holder::try_register_from_wasm(const std::vector<uint8_t>& wasm) {
   auto code_hash = fc::sha256::hash(reinterpret_cast<const char*>(wasm.data()), wasm.size());
   auto& registry = native_so_registry();
   auto it = registry.find(code_hash);
   if (it != registry.end()) {
      register_native_so(code_hash, it->second);
   }
}

void native_module_context_holder::init_native_contracts() {
   static bool initialized = false;
   if (initialized) return;
   initialized = true;

   namespace fs = std::filesystem;
#ifdef SYSIO_NATIVE_MODULE_RUNTIME_ENABLED
   fs::create_directories(chain::webassembly::native_module::native_runtime::code_dir());
#endif
   fs::path contracts_dir = NATIVE_CONTRACTS_DIR;

   // Auto-discover native contracts by scanning for *_native.so files.
   // Convention: <name>_native.so pairs with <name>.wasm in the same directory.
   std::error_code ec;
   for (auto& entry : fs::recursive_directory_iterator(contracts_dir, ec)) {
      if (!entry.is_regular_file()) continue;
      auto filename = entry.path().filename().string();
      const std::string suffix = "_native.so";
      if (filename.size() <= suffix.size() ||
          filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) != 0)
         continue;

      // Derive WASM path: strip _native.so, append .wasm
      auto base = filename.substr(0, filename.size() - suffix.size());
      fs::path wasm_path = entry.path().parent_path() / (base + ".wasm");
      if (!fs::exists(wasm_path)) {
         wlog("Native .so found but no matching WASM: {} (expected {})",
              entry.path().string(), wasm_path.string());
         continue;
      }

      std::ifstream wasm_file(wasm_path, std::ios::binary);
      if (!wasm_file) {
         wlog("Cannot open WASM file: {}", wasm_path.string());
         continue;
      }
      std::vector<uint8_t> wasm((std::istreambuf_iterator<char>(wasm_file)),
                                 std::istreambuf_iterator<char>());
      register_native_contract(wasm, entry.path());
      ilog("Registered native contract: {} -> {}", wasm_path.string(), entry.path().string());
   }

   if (native_so_registry().empty()) {
      FC_THROW("--native-module requested but no native contract .so files found in {}.\n"
               "Build native contracts with: ninja -C <build-dir> sysio.bios_native sysio.token_native ...\n"
               "Requires: -DNATIVE_CDT_DIR=/path/to/wire-cdt", contracts_dir.string());
   }
}

} // sysio::testing
