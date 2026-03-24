#pragma clang diagnostic ignored "-Weverything"
#include "/data/shared/code/wire/wire-sysio/contracts/sysio.uwrit/src/sysio.uwrit.cpp"
#include <sysio/datastream.hpp>
#include <sysio/name.hpp>


extern "C" {
  [[clang::import_name("action_data_size")]]
  uint32_t action_data_size();
  [[clang::import_name("read_action_data")]]
  uint32_t read_action_data(void*, uint32_t);
  __attribute__((weak))
  void __sysio_action_setconfig_uwrit(unsigned long long r, unsigned long long c) {
    size_t as = ::action_data_size();
    auto free_memory = [as](void* buf) { if (as >= 512) free(buf);};
    std::unique_ptr<void, decltype(free_memory)> buff{nullptr, free_memory};
    if (as > 0) {
      buff.reset(as >= 512 ? malloc(as) : alloca(as));
      ::read_action_data(buff.get(), as);
    }
    sysio::datastream<const char*> ds{(char*)buff.get(), as};
    uint32_t arg0; ds >> arg0;
    uint32_t arg1; ds >> arg1;
    uint32_t arg2; ds >> arg2;
    uint32_t arg3; ds >> arg3;
    uint32_t arg4; ds >> arg4;
    sysio::uwrit{sysio::name{r},sysio::name{c},ds}.setconfig(arg0, arg1, arg2, arg3, arg4);
  }
}


extern "C" {
  [[clang::import_name("action_data_size")]]
  uint32_t action_data_size();
  [[clang::import_name("read_action_data")]]
  uint32_t read_action_data(void*, uint32_t);
  __attribute__((weak))
  void __sysio_action_submituw_uwrit(unsigned long long r, unsigned long long c) {
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
    sysio::uwrit{sysio::name{r},sysio::name{c},ds}.submituw(arg0, arg1, arg2, arg3);
  }
}


extern "C" {
  [[clang::import_name("action_data_size")]]
  uint32_t action_data_size();
  [[clang::import_name("read_action_data")]]
  uint32_t read_action_data(void*, uint32_t);
  __attribute__((weak))
  void __sysio_action_confirmuw_uwrit(unsigned long long r, unsigned long long c) {
    size_t as = ::action_data_size();
    auto free_memory = [as](void* buf) { if (as >= 512) free(buf);};
    std::unique_ptr<void, decltype(free_memory)> buff{nullptr, free_memory};
    if (as > 0) {
      buff.reset(as >= 512 ? malloc(as) : alloca(as));
      ::read_action_data(buff.get(), as);
    }
    sysio::datastream<const char*> ds{(char*)buff.get(), as};
    uint64_t arg0; ds >> arg0;
    sysio::uwrit{sysio::name{r},sysio::name{c},ds}.confirmuw(arg0);
  }
}


extern "C" {
  [[clang::import_name("action_data_size")]]
  uint32_t action_data_size();
  [[clang::import_name("read_action_data")]]
  uint32_t read_action_data(void*, uint32_t);
  __attribute__((weak))
  void __sysio_action_expirelock_uwrit(unsigned long long r, unsigned long long c) {
    size_t as = ::action_data_size();
    auto free_memory = [as](void* buf) { if (as >= 512) free(buf);};
    std::unique_ptr<void, decltype(free_memory)> buff{nullptr, free_memory};
    if (as > 0) {
      buff.reset(as >= 512 ? malloc(as) : alloca(as));
      ::read_action_data(buff.get(), as);
    }
    sysio::datastream<const char*> ds{(char*)buff.get(), as};
    uint64_t arg0; ds >> arg0;
    sysio::uwrit{sysio::name{r},sysio::name{c},ds}.expirelock(arg0);
  }
}


extern "C" {
  [[clang::import_name("action_data_size")]]
  uint32_t action_data_size();
  [[clang::import_name("read_action_data")]]
  uint32_t read_action_data(void*, uint32_t);
  __attribute__((weak))
  void __sysio_action_distfee_uwrit(unsigned long long r, unsigned long long c) {
    size_t as = ::action_data_size();
    auto free_memory = [as](void* buf) { if (as >= 512) free(buf);};
    std::unique_ptr<void, decltype(free_memory)> buff{nullptr, free_memory};
    if (as > 0) {
      buff.reset(as >= 512 ? malloc(as) : alloca(as));
      ::read_action_data(buff.get(), as);
    }
    sysio::datastream<const char*> ds{(char*)buff.get(), as};
    uint64_t arg0; ds >> arg0;
    sysio::uwrit{sysio::name{r},sysio::name{c},ds}.distfee(arg0);
  }
}


extern "C" {
  [[clang::import_name("action_data_size")]]
  uint32_t action_data_size();
  [[clang::import_name("read_action_data")]]
  uint32_t read_action_data(void*, uint32_t);
  __attribute__((weak))
  void __sysio_action_updcltrl_uwrit(unsigned long long r, unsigned long long c) {
    size_t as = ::action_data_size();
    auto free_memory = [as](void* buf) { if (as >= 512) free(buf);};
    std::unique_ptr<void, decltype(free_memory)> buff{nullptr, free_memory};
    if (as > 0) {
      buff.reset(as >= 512 ? malloc(as) : alloca(as));
      ::read_action_data(buff.get(), as);
    }
    sysio::datastream<const char*> ds{(char*)buff.get(), as};
    sysio::name arg0; ds >> arg0;
    uint8_t arg1; ds >> arg1;
    sysio::asset arg2; ds >> arg2;
    bool arg3; ds >> arg3;
    sysio::uwrit{sysio::name{r},sysio::name{c},ds}.updcltrl(arg0, arg1, arg2, arg3);
  }
}


extern "C" {
  [[clang::import_name("action_data_size")]]
  uint32_t action_data_size();
  [[clang::import_name("read_action_data")]]
  uint32_t read_action_data(void*, uint32_t);
  __attribute__((weak))
  void __sysio_action_slash_uwrit(unsigned long long r, unsigned long long c) {
    size_t as = ::action_data_size();
    auto free_memory = [as](void* buf) { if (as >= 512) free(buf);};
    std::unique_ptr<void, decltype(free_memory)> buff{nullptr, free_memory};
    if (as > 0) {
      buff.reset(as >= 512 ? malloc(as) : alloca(as));
      ::read_action_data(buff.get(), as);
    }
    sysio::datastream<const char*> ds{(char*)buff.get(), as};
    sysio::name arg0; ds >> arg0;
    std::string arg1; ds >> arg1;
    sysio::uwrit{sysio::name{r},sysio::name{c},ds}.slash(arg0, arg1);
  }
}
