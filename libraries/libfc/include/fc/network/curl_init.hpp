// SPDX-License-Identifier: MIT
#pragma once

namespace fc {
   // Idempotent, thread-safe one-time init of libcurl's global state.
   // Safe to call from any plugin or thread; actual init happens exactly
   // once per process. Intentionally does not register a cleanup handler --
   // curl's global state is reclaimed at process exit.
   void ensure_libcurl_initialized();
}
