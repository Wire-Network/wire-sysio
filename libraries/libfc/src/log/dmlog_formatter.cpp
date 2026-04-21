#include <fc/log/dmlog_formatter.hpp>

#include <spdlog/details/log_msg.h>

#include <string_view>

namespace fc::log {

void dmlog_formatter::format(const spdlog::details::log_msg& msg, spdlog::memory_buf_t& dest) {
   constexpr std::string_view prefix{"DMLOG "};
   dest.append(prefix.data(), prefix.data() + prefix.size());
   dest.append(msg.payload.data(), msg.payload.data() + msg.payload.size());
   dest.push_back('\n');
}

std::unique_ptr<spdlog::formatter> dmlog_formatter::clone() const {
   return std::make_unique<dmlog_formatter>();
}

} // namespace fc::log
