#pragma once
#include <stdexcept>
#include <string>

inline constexpr auto native_probe_message = "wire-native-libcxx";

/// Shared exception type verifies RTTI and unwinding across a dlopen boundary.
class native_probe_error : public std::runtime_error {
public:
   native_probe_error() : std::runtime_error(native_probe_message) {}
};

using native_callback = void (*)();
using native_probe_function = std::string* (*)(native_callback, bool*);
