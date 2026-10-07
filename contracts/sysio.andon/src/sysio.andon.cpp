#include <sysio.andon/sysio.andon.hpp>
#include <sysio/print.hpp>
#include <sysio/system.hpp>

namespace sysio {
void andon::pull(std::string reason) {
   require_auth(get_self());
   cord_t cord(get_self());
   if (cord.get_or_default(cord_state{}).pulled) {
      print("sysio.andon::pull: the cord is already pulled; nothing changes\n");
      return;
   }
   reason.resize(std::min(reason.size(), size_t{MAX_TEXT_BYTES}));
   cord.set(cord_state{true, current_time_point(), std::move(reason)}, SYSTEM_ACCOUNT);
}

void andon::clear(std::string note) {
   require_auth(get_self());
   cord_t cord(get_self());
   if (!cord.get_or_default(cord_state{}).pulled) {
      print("sysio.andon::clear: the cord is not pulled; nothing changes\n");
      return;
   }
   note.resize(std::min(note.size(), size_t{MAX_TEXT_BYTES}));
   cord.set(cord_state{false, current_time_point(), std::move(note)}, SYSTEM_ACCOUNT);
}
} // namespace sysio
