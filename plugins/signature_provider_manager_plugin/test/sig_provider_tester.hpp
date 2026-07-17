#pragma once

/**
 * Shared appbase fixture for the signature_provider_manager_plugin test
 * suites (test_create_provider_specs.cpp, test_spec_retain_claim.cpp -- one
 * binary, multiple TUs, so the fixture lives here instead of being repeated
 * per-TU in anonymous namespaces).
 *
 * Each test case owns one `sig_provider_tester` (a `scoped_app` + accessor)
 * and must pair it with
 *   auto clean_app = gsl_lite::finally([] { appbase::application::reset_app_singleton(); });
 * because `app()` is process-global.
 */

#include <boost/test/unit_test.hpp>

#include <sysio/signature_provider_manager_plugin/signature_provider_manager_plugin.hpp>

#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace sysio::sigprov::test {

/**
 * Sig provider tester app resources
 */
struct sig_provider_tester {

   appbase::scoped_app app{};

   signature_provider_manager_plugin& plugin() { return app->get_plugin<signature_provider_manager_plugin>(); }
};

/**
 * Creates a tester/app scoped instance initialized with the given argv tail.
 *
 * @param args additional command-line arguments (e.g. `--signature-provider`, `<spec>`)
 * @return `unique_ptr<sig_provider_tester>` with the manager plugin initialized
 */
inline std::unique_ptr<sig_provider_tester> create_app(const std::vector<std::string>& args) {
   auto tester = std::make_unique<sig_provider_tester>();

   // Build argv as vector<char*> pointing to the underlying string buffers
   std::vector<const char*> argv;
   argv.reserve(args.size() + 1);
   argv.push_back("test_signature_provider_manager_plugin"); // program name
   for (auto& s : args) {
      argv.push_back(s.c_str());
   }

   BOOST_CHECK(tester->app->initialize<sysio::signature_provider_manager_plugin>(argv.size(),
                                                                                 const_cast<char**>(argv.data())));

   return tester;
}

/**
 * Variadic convenience overload of `create_app`.
 *
 * @tparam Args pack of `std::string` arguments forwarded as the argv tail
 * @return `unique_ptr<sig_provider_tester>`
 */
template <typename... Args>
   requires((std::same_as<std::decay_t<Args>, std::string>) && ...)
std::unique_ptr<sig_provider_tester> create_app(Args&&... extra_args) {
   std::vector<std::string> args_vec = {std::forward<Args>(extra_args)...};
   return create_app(args_vec);
}

} // namespace sysio::sigprov::test
