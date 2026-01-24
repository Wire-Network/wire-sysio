#include <fc/log/dmlog_sink.hpp>
#include <fc/exception/exception.hpp>

#include <unistd.h>
#include <signal.h>
#include <cstdio>


namespace fc {

   dmlog_sink_mt::dmlog_sink_mt(const std::string& file) {
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
      // log_msg is a struct containing the log entry info like level, timestamp, thread id etc.
      // msg.payload (before v1.3.0: msg.raw) contains pre formatted log

      // If needed (very likely but not mandatory), the sink formats the message before sending it to its final destination:
      // spdlog::memory_buf_t formatted;
      // spdlog::sinks::base_sink<Mutex>::formatter_->format(msg, formatted);
      // std::cout << fmt::to_string(formatted);

      std::string message = "DMLOG " + fmt::to_string(msg.payload) + "\n";

      auto remaining_size = message.size();
      auto message_ptr = message.c_str();
      while (!is_stopped && remaining_size) {
         auto written = fwrite(message_ptr, sizeof(char), remaining_size, out);

         // EINTR shouldn't happen anymore, but keep this detection, just in case.
         if(written == 0 && errno != EINTR) {
            is_stopped = true;
         }

         if(written != remaining_size) {
            fprintf(stderr, "DMLOG FPRINTF_FAILED failed written=%lu remaining=%lu %d %s\n", written, remaining_size, ferror(out), strerror(errno));
            clearerr(out);
         }

         if(is_stopped) {
            fprintf(stderr, "DMLOG FPRINTF_FAILURE_TERMINATED\n");
            // Depending on the error, we might have already gotten a SIGPIPE
            // An extra signal is harmless, though.  Use a process targeted
            // signal (not raise) because the SIGTERM may be blocked in this
            // thread.
            kill(getpid(), SIGTERM);
         }

         message_ptr = &message_ptr[written];
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
