#pragma once
#include <fc/spdlog.hpp>

#include <memory>

namespace fc::log {

/// spdlog formatter producing "DMLOG " + msg.payload + "\n", matching the
/// deep-mind tracer wire format consumed by dfuse postprocessing tools.
/// Usable on any sink via sink->set_formatter().
class dmlog_formatter final : public spdlog::formatter {
public:
   dmlog_formatter() = default;

   void format(const spdlog::details::log_msg& msg, spdlog::memory_buf_t& dest) override;
   std::unique_ptr<spdlog::formatter> clone() const override;
};

} // namespace fc::log
