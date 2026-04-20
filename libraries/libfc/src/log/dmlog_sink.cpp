#include <fc/log/dmlog_sink.hpp>
#include <fc/log/dmlog_formatter.hpp>
#include <fc/exception/exception.hpp>

#include <spdlog/details/log_msg.h>

#include <unistd.h>
#include <signal.h>
#include <cstdio>
#include <print>


namespace fc {

   dmlog_sink_mt::dmlog_sink_mt(const std::string& file)
      : spdlog::sinks::base_sink<std::mutex>(std::make_unique<fc::log::dmlog_formatter>())
   {
      if (file.empty() || file == "-" || file == "-stdout") {
         out = stdout;
      } else if (file == "-stderr") {
         out = stderr;
      } else {
         out = std::fopen(file.c_str(), "a");
         if (out) {
            std::setbuf(out, nullptr);
            owns_out = true;
         } else {
            FC_THROW("Failed to open deep mind log file {}", file);
         }
      }
   }

   dmlog_sink_mt::~dmlog_sink_mt() {
      if (owns_out) {
         std::fclose(out);
      }
   }

   void dmlog_sink_mt::sink_it_(const spdlog::details::log_msg& msg) {
      spdlog::memory_buf_t formatted;
      formatter_->format(msg, formatted);

      auto remaining_size = formatted.size();
      const char* message_ptr = formatted.data();
      while (!is_stopped && remaining_size) {
         auto written = fwrite(message_ptr, sizeof(char), remaining_size, out);

         // EINTR shouldn't happen anymore, but keep this detection, just in case.
         if(written == 0 && errno != EINTR) {
            is_stopped = true;
         }

         if(written != remaining_size) {
            std::println(stderr, "DMLOG WRITE_FAILED written={} remaining={} {} {}", written, remaining_size, ferror(out), strerror(errno));
            clearerr(out);
         }

         if(is_stopped) {
            std::println(stderr, "DMLOG WRITE_FAILURE_TERMINATED");
            // Depending on the error, we might have already gotten a SIGPIPE
            // An extra signal is harmless, though.  Use a process targeted
            // signal (not raise) because the SIGTERM may be blocked in this
            // thread.
            kill(getpid(), SIGTERM);
         }

         message_ptr += written;
         remaining_size -= written;
      }
      // attempt a flush, ignore any error
      if (!is_stopped)
         fflush(out);
   }

   void dmlog_sink_mt::flush_() {
      // always flushed
   }

}
