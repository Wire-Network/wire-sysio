#include <version>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

static_assert(_LIBCPP_VERSION == 230103, "Wire requires libc++ 23.1.3");

extern "C" int wire_c_runtime_probe(void);

namespace {
constexpr auto probe_message = "wire-libcxx-runtime";
/// Polymorphic exception exercises RTTI as well as unwinding across a thread.
class runtime_probe_error final : public std::runtime_error {
public:
   runtime_probe_error() : std::runtime_error(probe_message) {}
};
}

/// Verify the selected standard library supports threads, exceptions and RTTI.
int main() {
   if (wire_c_runtime_probe() != 1)
      return 3;
   // GCC must call libc++'s pointer-valued future state move(), rather than
   // folding it like std::move and returning the state allocation itself.
   int value = 42;
   std::promise<int*> promised;
   auto result = promised.get_future();
   std::thread producer([&] { promised.set_value(&value); });
   auto* received = result.get();
   producer.join();
   if (received != &value)
      return 4;
   // Exercise out-of-line string growth, including allocations beyond SSO.
   // On Darwin these calls must stay in the pinned runtime.
   std::string growing;
   for (int i = 0; i < 1024; ++i)
      growing.append("wire");
   if (growing.size() != 4096)
      return 5;
   for (std::size_t i = 0; i < growing.size(); ++i)
      if (growing[i] != "wire"[i % 4])
         return 5;
   std::exception_ptr error;
   std::thread worker([&] {
      try {
         auto message = std::make_unique<std::string>(probe_message);
         if (!message->empty())
            throw runtime_probe_error();
      } catch (...) {
         error = std::current_exception();
      }
   });
   worker.join();
   if (!error)
      return 1;
   try {
      std::rethrow_exception(error);
   } catch (const std::exception& caught) {
      if (!dynamic_cast<const runtime_probe_error*>(&caught) ||
          std::string(caught.what()) != probe_message)
         return 2;
   }
   std::cout << "libc++ " << _LIBCPP_VERSION << ": futures, exceptions, RTTI and threads passed\n";
}
