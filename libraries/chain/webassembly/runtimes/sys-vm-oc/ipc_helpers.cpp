#include <sysio/chain/webassembly/sys-vm-oc/ipc_helpers.hpp>
#include <sysio/chain/exceptions.hpp>

namespace sysio { namespace chain { namespace sysvmoc {

static constexpr size_t max_message_size = 8192;
static constexpr size_t max_num_fds = 4;

std::tuple<bool, sysvmoc_message, std::vector<wrapped_fd>> read_message_with_fds(boost::asio::local::datagram_protocol::socket& s) {
   // read_message_with_fds() is intended to be blocking, and sockets it is used with are never explicitly set to non-blocking mode.
   // But when doing an async_wait() on an asio socket, asio will set the underlying file descriptor to non-blocking mode.
   // It's not clear why, but in some cases after async_wait() indicates readiness, a recvmsg() on the socket can fail with
   // EAGAIN if the file descriptor is still in non-blocking mode (as asio had set it to).
   // Always set the file descriptor to blocking mode before performing the recvmsg()
   boost::system::error_code ec;
   s.native_non_blocking(false, ec);
   if(ec)
      wlog("Failed to set socket's native blocking mode");

   return read_message_with_fds(s.native_handle());
}

std::tuple<bool, sysvmoc_message, std::vector<wrapped_fd>> read_message_with_fds(int fd) {
   char buff[max_message_size];

   struct msghdr msg = {};
   struct cmsghdr* cmsg;

   sysvmoc_message message;
   std::vector<wrapped_fd> fds;

   struct iovec io = {
      .iov_base = buff,
      .iov_len = sizeof(buff)
   };
   union {
      char buf[CMSG_SPACE(max_num_fds * sizeof(int))];
      struct cmsghdr align;
   } u;

   msg.msg_iov = &io;
   msg.msg_iovlen = 1;
   msg.msg_control = u.buf;
   msg.msg_controllen = sizeof(u.buf);

   int red;
   do {
      red = recvmsg(fd, &msg, 0);
   } while(red == -1 && errno == EINTR);
   if(red < 1 || static_cast<unsigned>(red) >= sizeof(buff))
      return {false, message, std::move(fds)};
   
   try {
      fc::datastream<char*> ds(buff, red);
      fc::raw::unpack(ds, message);
   }
   catch(...) {
      return {false, message, std::move(fds)};
   }

   if(msg.msg_controllen) {
      cmsg = CMSG_FIRSTHDR(&msg);
      unsigned num_of_fds = (cmsg->cmsg_len - CMSG_LEN(0))/sizeof(int);
      if(num_of_fds > max_num_fds)
         return {false, message, std::move(fds)};
      int* fd_ptr = (int*)CMSG_DATA(cmsg);
      for(unsigned i = 0; i < num_of_fds; ++i)
         fds.push_back(*fd_ptr++);
   }

   return {true, message, std::move(fds)};
}

bool write_message_with_fds(boost::asio::local::datagram_protocol::socket& s, const sysvmoc_message& message, const std::vector<wrapped_fd>& fds) {
   return write_message_with_fds(s.native_handle(), message, fds);
}

bool write_message_with_fds(int fd_to_send_to, const sysvmoc_message& message, const std::vector<wrapped_fd>& fds) {
   struct msghdr msg = {};
   struct cmsghdr* cmsg;

   size_t sz = fc::raw::pack_size(message);
   if(sz > max_message_size)
      return false;
   char buff[max_message_size];
   try {
      fc::datastream<char*> ds(buff, max_message_size);
      fc::raw::pack(ds, message);
   }
   catch(...) {
      return false;
   }

   if(fds.size() > max_num_fds)
      return false;

   struct iovec io = {
      .iov_base = buff,
      .iov_len = sz
   };
   union {
      char buf[CMSG_SPACE(max_num_fds * sizeof(int))] = {};
      struct cmsghdr align;
   } u;

   msg.msg_iov = &io;
   msg.msg_iovlen = 1;
   if(fds.size()) {
      msg.msg_control = u.buf;
      msg.msg_controllen = sizeof(u.buf);
      cmsg = CMSG_FIRSTHDR(&msg);
      cmsg->cmsg_level = SOL_SOCKET;
      cmsg->cmsg_type = SCM_RIGHTS;
      cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
      unsigned char* p = CMSG_DATA(cmsg);
      for(const wrapped_fd& fd : fds) {
         int thisfd = fd;
         memcpy(p, &thisfd, sizeof(thisfd));
         p += sizeof(thisfd);
      }
      msg.msg_controllen = cmsg->cmsg_len;
   }

   int wrote;
   do {
      wrote = sendmsg(fd_to_send_to, &msg, 0);
   } while(wrote == -1 && errno == EINTR);

   return wrote >= 0;
}

std::vector<uint8_t> vector_for_memfd(const wrapped_fd& memfd) {
   struct stat st;
   FC_ASSERT(fstat(memfd, &st) == 0, "failed to get memfd size");

   if(st.st_size == 0)
      return std::vector<uint8_t>();

   uint8_t* p = (uint8_t*)mmap(nullptr, st.st_size, PROT_READ|PROT_WRITE, MAP_SHARED, memfd, 0);
   FC_ASSERT(p != MAP_FAILED, "failed to map memfd");
   std::vector<uint8_t> ret(p, p+st.st_size);
   munmap(p, st.st_size);
   return ret;
}

}}}
