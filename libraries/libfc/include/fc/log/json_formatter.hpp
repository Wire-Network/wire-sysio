#pragma once
#include <fc/spdlog.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace fc {

/// spdlog formatter that emits one JSON object per log record, terminated
/// by '\n'. Fields: ts (UTC microseconds), lvl, thread, logger, file, line,
/// func, msg, (optional) extra. Usable on any sink -- attach via
/// sink->set_formatter(std::make_unique<fc::json_formatter>(...)).
class json_formatter final : public spdlog::formatter {
public:
   explicit json_formatter(std::map<std::string, std::string> extra_fields = {});

   void format(const spdlog::details::log_msg& msg, spdlog::memory_buf_t& dest) override;
   std::unique_ptr<spdlog::formatter> clone() const override;

private:
   std::map<std::string, std::string> extra_fields_;
};

} // namespace fc
