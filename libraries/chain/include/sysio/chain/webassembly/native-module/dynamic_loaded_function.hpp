#pragma once

#include <dlfcn.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

namespace sysio::chain::webassembly::native_module {

/// RAII wrapper around dlopen/dlsym for loading a single symbol from a shared library.
/// Used exclusively by the native-module WASM runtime to load the `apply` entry point
/// from natively-compiled contract .so files. Move-only; calls dlclose on destruction.
class dynamic_loaded_function {
public:
   dynamic_loaded_function() = default;

   explicit dynamic_loaded_function(const std::filesystem::path& filename, const char* symbol) {
      handle_ = dlopen(filename.c_str(), RTLD_NOW | RTLD_LOCAL);
      if (!handle_)
         throw std::runtime_error(std::string("dlopen failed for ") + filename.string() + ": " + dlerror());
      func_ = dlsym(handle_, symbol);
      if (!func_) {
         dlclose(handle_);
         handle_ = nullptr;
         throw std::runtime_error(std::string("dlsym failed for '") + symbol + "' in " + filename.string() + ": " + dlerror());
      }
   }

   ~dynamic_loaded_function() {
      if (handle_)
         dlclose(handle_);
   }

   dynamic_loaded_function(dynamic_loaded_function&& o) noexcept
      : handle_(std::exchange(o.handle_, nullptr))
      , func_(std::exchange(o.func_, nullptr))
   {}

   dynamic_loaded_function& operator=(dynamic_loaded_function&& o) noexcept {
      if (this != &o) {
         if (handle_)
            dlclose(handle_);
         handle_ = std::exchange(o.handle_, nullptr);
         func_ = std::exchange(o.func_, nullptr);
      }
      return *this;
   }

   dynamic_loaded_function(const dynamic_loaded_function&) = delete;
   dynamic_loaded_function& operator=(const dynamic_loaded_function&) = delete;

   template <typename... Args>
   void exec(Args&&... args) const {
      using fn_t = void(*)(Args...);
      reinterpret_cast<fn_t>(func_)(std::forward<Args>(args)...);
   }

   explicit operator bool() const { return func_ != nullptr; }

private:
   void* handle_ = nullptr;
   void* func_   = nullptr;
};

} // sysio::chain::webassembly::native_module
