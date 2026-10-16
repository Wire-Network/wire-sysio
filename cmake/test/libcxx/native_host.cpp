#include "native_probe.hpp"
#include <dlfcn.h>
#include <iostream>
#include <memory>

/// Verify two-way exception propagation, RTTI, RAII and C++ allocation ownership.
int main(int argc, char** argv) {
   if (argc != 2)
      return 1;
   void* module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
   if (!module) {
      std::cerr << dlerror() << '\n';
      return 1;
   }
   auto probe = reinterpret_cast<native_probe_function>(dlsym(module, "native_probe"));
   if (!probe)
      return 1;
   bool destroyed = false;
   for (bool from_host : {false, true}) {
      try {
         probe(from_host ? +[] { throw native_probe_error(); } : nullptr,
               from_host ? &destroyed : nullptr);
         return 1;
      } catch (const std::exception& error) {
         // The standard exception RTTI is owned by shared libc++abi; the local
         // derived type is intentionally not exported by the host executable.
         if (!dynamic_cast<const std::runtime_error*>(&error) ||
             std::string(error.what()) != native_probe_message)
            return 1;
      }
   }
   if (!destroyed)
      return 1;
   {
      std::unique_ptr<std::string> message(probe(nullptr, &destroyed));
      if (*message != native_probe_message)
         return 1;
   }
   dlclose(module);
   std::cout << "Native libc++ ABI and unwinding passed\n";
}
