#pragma once
#include <fc/log/logger.hpp>
#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/base_sink.h>
#include <string>
#include <mutex>
#include <cstdio>

namespace fc {

   /**
    * Specialized sink for deep mind tracer that sends log messages
    * through `stdout` correctly formatted for latter consumption by
    * deep mind postprocessing tools from dfuse.
    */
   class dmlog_sink_mt : public spdlog::sinks::base_sink<std::mutex> {
   public:
      explicit dmlog_sink_mt(const std::string& file);
      virtual ~dmlog_sink_mt();

   protected:
      void sink_it_(const spdlog::details::log_msg& msg) override;
      void flush_() override;
   private:
      bool is_stopped = false;
      FILE* out = nullptr;
      bool owns_out = false;
   };

} // namespece fc
