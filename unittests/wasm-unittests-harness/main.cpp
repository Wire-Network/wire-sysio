#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include <cstdlib>
#include <filesystem>
#include <print>
#include <boost/program_options.hpp>
#ifndef _WIN32
#include <sys/wait.h>
#endif

// Simple harness that runs one or more WASI .wasm test modules (built with Boost.Test)
// using an external WASI runtime (default: wasmtime). Each module is executed with
// full disk access by preopening the root directory.
//
// Usage:
//   wasm-unittests-harness [--runner <cmd>] <test1.wasm> [<test2.wasm> ...]
//
// Notes:
// - By default the harness uses "wasmtime" (on PATH). Override with --runner or
//   environment variable WASI_RUNNER.
// - Full disk access is provided by passing "--dir=/" to the runtime.
// - The harness returns non-zero if any of the modules return a non-zero exit code.
// - Additional arguments intended for Boost.Test inside the WASM are not handled by
//   this harness; invoke with your own wrapper if needed.

namespace fs = std::filesystem;
namespace po = boost::program_options;

static int run_one(const std::string& runner, const fs::path& wasm_path) {
  // Construct command. Prefer a conservative set of flags for wasmtime.
  // We attempt: <runner> run --dir=/ <wasm_path>
  std::string cmd = runner + " run --dir=/tmp \"" + wasm_path.string() + "\"";
  std::println(std::cout, "[harness] running: {}", cmd);
  int rc = std::system(cmd.c_str());
  if (rc == -1) {
    std::perror("[harness] system()");
    return 127;
  }
#ifdef _WIN32
  // On Windows, system returns the exit code directly.
  return rc;
#else
  if (WIFEXITED(rc)) {
    return WEXITSTATUS(rc);
  } else if (WIFSIGNALED(rc)) {
    int sig = WTERMSIG(rc);
    std::println(std::cerr, "[harness] process terminated by signal: {}", sig);
    return 128 + sig;
  } else {
    return rc != 0; // non-zero
  }
#endif
}

int main(int argc, char** argv) {
  try {
    po::options_description desc("Allowed options");
    desc.add_options()
      ("help,h", "produce help message")
      ("runner,r", po::value<std::string>(), "WASI runtime command (default: wasmtime or WASI_RUNNER env var)")
      ("wasm-files", po::value<std::vector<std::string>>(), "WASM test files to execute");

    po::positional_options_description pos_desc;
    pos_desc.add("wasm-files", -1);

    po::variables_map vm;
    po::store(po::command_line_parser(argc, argv)
                .options(desc)
                .positional(pos_desc)
                .run(), vm);
    po::notify(vm);

    if (vm.count("help")) {
      std::println(std::cout, "Usage: {} [--runner <cmd>] <test1.wasm> [<test2.wasm> ...]", argv[0]);
      std::cout << desc << std::endl;
      return 0;
    }

    std::string runner;
    if (vm.count("runner")) {
      runner = vm["runner"].as<std::string>();
    } else {
      const char* env_runner = std::getenv("WASI_RUNNER");
      if (env_runner && *env_runner) {
        runner = env_runner;
      } else {
        runner = "wasmtime"; // default
      }
    }

    std::vector<fs::path> wasm_paths;
    if (vm.count("wasm-files")) {
      const auto& files = vm["wasm-files"].as<std::vector<std::string>>();
      for (const auto& file : files) {
        wasm_paths.emplace_back(file);
      }
    }

    if (wasm_paths.empty()) {
      std::println(std::cerr, "[harness] no .wasm files provided");
      return 2;
    }

    int failures = 0;
    for (const auto& p : wasm_paths) {
      if (!fs::exists(p)) {
        std::println(std::cerr, "[harness] file not found: {}", p.string());
        ++failures;
        continue;
      }
      if (!fs::is_regular_file(p)) {
        std::println(std::cerr, "[harness] not a regular file: {}", p.string());
        ++failures;
        continue;
      }
      int rc = run_one(runner, p);
      if (rc != 0) {
        std::println(std::cerr, "[harness] FAILED (exit={}): {}", rc, p.string());
        ++failures;
      } else {
        std::println(std::cout, "[harness] PASSED: {}", p.string());
      }
    }

    if (failures) {
      std::println(std::cerr, "[harness] summary: {} module(s) failed", failures);
      return 1;
    }
    std::println(std::cout, "[harness] summary: all modules passed");
    return 0;

  } catch (const po::error& e) {
    std::println(std::cerr, "[harness] error: {}", e.what());
    return 2;
  } catch (const std::exception& e) {
    std::println(std::cerr, "[harness] exception: {}", e.what());
    return 2;
  }
}
