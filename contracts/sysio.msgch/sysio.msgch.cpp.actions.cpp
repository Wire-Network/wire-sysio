#pragma clang diagnostic ignored "-Weverything"
#include "/data/shared/code/wire/wire-sysio/contracts/sysio.msgch/src/sysio.msgch.cpp"
#include <sysio/datastream.hpp>
#include <sysio/name.hpp>


extern "C" {
  [[clang::import_name("action_data_size")]]
  uint32_t action_data_size();
  [[clang::import_name("read_action_data")]]
  uint32_t read_action_data(void*, uint32_t);
  __attribute__((weak))
  void __sysio_action_crank_msgch(unsigned long long r, unsigned long long c) {
    size_t as = ::action_data_size();
    auto free_memory = [as](void* buf) { if (as >= 512) free(buf);};
    std::unique_ptr<void, decltype(free_memory)> buff{nullptr, free_memory};
    if (as > 0) {
      buff.reset(as >= 512 ? malloc(as) : alloca(as));
      ::read_action_data(buff.get(), as);
    }
    sysio::datastream<const char*> ds{(char*)buff.get(), as};
    sysio::msgch{sysio::name{r},sysio::name{c},ds}.crank();
  }
}


extern "C" {
  [[clang::import_name("action_data_size")]]
  uint32_t action_data_size();
  [[clang::import_name("read_action_data")]]
  uint32_t read_action_data(void*, uint32_t);
  __attribute__((weak))
  void __sysio_action_createreq_msgch(unsigned long long r, unsigned long long c) {
    size_t as = ::action_data_size();
    auto free_memory = [as](void* buf) { if (as >= 512) free(buf);};
    std::unique_ptr<void, decltype(free_memory)> buff{nullptr, free_memory};
    if (as > 0) {
      buff.reset(as >= 512 ? malloc(as) : alloca(as));
      ::read_action_data(buff.get(), as);
    }
    sysio::datastream<const char*> ds{(char*)buff.get(), as};
    uint64_t arg0; ds >> arg0;
    sysio::msgch{sysio::name{r},sysio::name{c},ds}.createreq(arg0);
  }
}


extern "C" {
  [[clang::import_name("action_data_size")]]
  uint32_t action_data_size();
  [[clang::import_name("read_action_data")]]
  uint32_t read_action_data(void*, uint32_t);
  __attribute__((weak))
  void __sysio_action_deliver_msgch(unsigned long long r, unsigned long long c) {
    size_t as = ::action_data_size();
    auto free_memory = [as](void* buf) { if (as >= 512) free(buf);};
    std::unique_ptr<void, decltype(free_memory)> buff{nullptr, free_memory};
    if (as > 0) {
      buff.reset(as >= 512 ? malloc(as) : alloca(as));
      ::read_action_data(buff.get(), as);
    }
    sysio::datastream<const char*> ds{(char*)buff.get(), as};
    sysio::name arg0; ds >> arg0;
    uint64_t arg1; ds >> arg1;
    sysio::checksum256 arg2; ds >> arg2;
    sysio::checksum256 arg3; ds >> arg3;
    uint32_t arg4; ds >> arg4;
    std::vector<char> arg5; ds >> arg5;
    sysio::msgch{sysio::name{r},sysio::name{c},ds}.deliver(arg0, arg1, arg2, arg3, arg4, arg5);
  }
}


extern "C" {
  [[clang::import_name("action_data_size")]]
  uint32_t action_data_size();
  [[clang::import_name("read_action_data")]]
  uint32_t read_action_data(void*, uint32_t);
  __attribute__((weak))
  void __sysio_action_evalcons_msgch(unsigned long long r, unsigned long long c) {
    size_t as = ::action_data_size();
    auto free_memory = [as](void* buf) { if (as >= 512) free(buf);};
    std::unique_ptr<void, decltype(free_memory)> buff{nullptr, free_memory};
    if (as > 0) {
      buff.reset(as >= 512 ? malloc(as) : alloca(as));
      ::read_action_data(buff.get(), as);
    }
    sysio::datastream<const char*> ds{(char*)buff.get(), as};
    uint64_t arg0; ds >> arg0;
    sysio::msgch{sysio::name{r},sysio::name{c},ds}.evalcons(arg0);
  }
}


extern "C" {
  [[clang::import_name("action_data_size")]]
  uint32_t action_data_size();
  [[clang::import_name("read_action_data")]]
  uint32_t read_action_data(void*, uint32_t);
  __attribute__((weak))
  void __sysio_action_processmsg_msgch(unsigned long long r, unsigned long long c) {
    size_t as = ::action_data_size();
    auto free_memory = [as](void* buf) { if (as >= 512) free(buf);};
    std::unique_ptr<void, decltype(free_memory)> buff{nullptr, free_memory};
    if (as > 0) {
      buff.reset(as >= 512 ? malloc(as) : alloca(as));
      ::read_action_data(buff.get(), as);
    }
    sysio::datastream<const char*> ds{(char*)buff.get(), as};
    uint64_t arg0; ds >> arg0;
    sysio::msgch{sysio::name{r},sysio::name{c},ds}.processmsg(arg0);
  }
}


extern "C" {
  [[clang::import_name("action_data_size")]]
  uint32_t action_data_size();
  [[clang::import_name("read_action_data")]]
  uint32_t read_action_data(void*, uint32_t);
  __attribute__((weak))
  void __sysio_action_queueout_msgch(unsigned long long r, unsigned long long c) {
    size_t as = ::action_data_size();
    auto free_memory = [as](void* buf) { if (as >= 512) free(buf);};
    std::unique_ptr<void, decltype(free_memory)> buff{nullptr, free_memory};
    if (as > 0) {
      buff.reset(as >= 512 ? malloc(as) : alloca(as));
      ::read_action_data(buff.get(), as);
    }
    sysio::datastream<const char*> ds{(char*)buff.get(), as};
    uint64_t arg0; ds >> arg0;
    uint16_t arg1; ds >> arg1;
    std::vector<char> arg2; ds >> arg2;
    sysio::msgch{sysio::name{r},sysio::name{c},ds}.queueout(arg0, arg1, arg2);
  }
}


extern "C" {
  [[clang::import_name("action_data_size")]]
  uint32_t action_data_size();
  [[clang::import_name("read_action_data")]]
  uint32_t read_action_data(void*, uint32_t);
  __attribute__((weak))
  void __sysio_action_buildenv_msgch(unsigned long long r, unsigned long long c) {
    size_t as = ::action_data_size();
    auto free_memory = [as](void* buf) { if (as >= 512) free(buf);};
    std::unique_ptr<void, decltype(free_memory)> buff{nullptr, free_memory};
    if (as > 0) {
      buff.reset(as >= 512 ? malloc(as) : alloca(as));
      ::read_action_data(buff.get(), as);
    }
    sysio::datastream<const char*> ds{(char*)buff.get(), as};
    uint64_t arg0; ds >> arg0;
    sysio::msgch{sysio::name{r},sysio::name{c},ds}.buildenv(arg0);
  }
}
