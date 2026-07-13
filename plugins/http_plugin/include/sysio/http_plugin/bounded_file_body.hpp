#pragma once

#include <boost/asio/buffer.hpp>
#include <boost/assert.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/file.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/optional.hpp>

#include <cstdint>
#include <utility>

namespace sysio {

/**
 * An HTTP message body backed by a contiguous byte range of a file.
 *
 * boost::beast::http::file_body always serializes from the file's current read position
 * through end-of-file: seeking only moves the start, so a response built from it over-sends
 * past the end of a requested Range. Beast has no built-in range-limited file body, so this
 * Body type carries an explicit [first, first + size) window and its writer stops at the
 * window's end.
 *
 * Satisfies Beast's Body named requirements for serialization (writer + size); parsing
 * (reader) is intentionally not provided -- the http_plugin only sends files.
 */
struct bounded_file_body {
   class value_type {
      friend struct bounded_file_body;

      boost::beast::file file_;
      std::uint64_t      first_ = 0; ///< file offset of the first byte to serialize
      std::uint64_t      size_  = 0; ///< number of bytes to serialize

   public:
      value_type() = default;
      value_type(value_type&&) = default;
      value_type& operator=(value_type&&) = default;

      /// Returns true if the file is open.
      bool is_open() const { return file_.is_open(); }

      /// Returns the number of bytes the body serializes (drives Content-Length).
      std::uint64_t size() const { return size_; }

      /// Open the file at @p path for reading; the body initially covers the whole file.
      void open(const char* path, boost::beast::file_mode mode, boost::system::error_code& ec) {
         file_.open(path, mode, ec);
         if(ec)
            return;
         first_ = 0;
         size_  = file_.size(ec);
         if(ec)
            close();
      }

      /**
       * Restrict the body to @p len bytes starting at file offset @p first.
       * The caller is responsible for clamping the window to the file size.
       */
      void set_range(std::uint64_t first, std::uint64_t len) {
         first_ = first;
         size_  = len;
      }

      /// Close the file if open.
      void close() {
         boost::system::error_code ignored;
         file_.close(ignored);
      }
   };

   /// Returns the serialized size of the body; Beast uses this for Content-Length.
   static std::uint64_t size(const value_type& body) { return body.size(); }

   /// Serializer adapter: feeds the serializer fixed-size chunks of the bounded window.
   class writer {
      /// Chunk size for file reads, matching Beast's own file-body buffer size.
      static constexpr std::size_t file_buffer_size = 4096;

      value_type&   body_;
      std::uint64_t remain_; ///< bytes of the window not yet handed to the serializer
      char          buf_[file_buffer_size];

   public:
      using const_buffers_type = boost::asio::const_buffer;

      template<bool isRequest, class Fields>
      writer(boost::beast::http::header<isRequest, Fields>&, value_type& b)
         : body_(b)
         , remain_(b.size_) {
         BOOST_ASSERT(body_.file_.is_open());
      }

      void init(boost::system::error_code& ec) {
         // Seek here rather than in the constructor so a failure surfaces through ec.
         body_.file_.seek(body_.first_, ec);
      }

      boost::optional<std::pair<const_buffers_type, bool>> get(boost::system::error_code& ec) {
         const auto amount = remain_ > sizeof(buf_) ? sizeof(buf_) : static_cast<std::size_t>(remain_);
         if(amount == 0) {
            ec = {};
            return boost::none;
         }

         const auto nread = body_.file_.read(buf_, amount, ec);
         if(ec)
            return boost::none;
         if(nread == 0) {
            // EOF before the advertised window was exhausted (file truncated mid-send).
            ec = boost::beast::http::error::short_read;
            return boost::none;
         }

         BOOST_ASSERT(nread <= remain_);
         remain_ -= nread;
         ec = {};
         return {{const_buffers_type{buf_, nread}, remain_ > 0}};
      }
   };
};

} // namespace sysio
