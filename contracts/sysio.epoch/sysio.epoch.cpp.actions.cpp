#pragma clang diagnostic ignored "-Weverything"
#include "/data/shared/code/wire/wire-sysio/contracts/sysio.epoch/src/sysio.epoch.cpp"
#include <sysio/datastream.hpp>
#include <sysio/name.hpp>


extern "C" {
  [[clang::import_name("action_data_size")]]
  uint32_t action_data_size();
  [[clang::import_name("read_action_data")]]
  uint32_t read_action_data(void*, uint32_t);
  __attribute__((weak))
  void __sysio_action_setconfig_epoch(unsigned long long r, unsigned long long c) {
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
    uint32_t arg5; ds >> arg5;
    sysio::epoch{sysio::name{r},sysio::name{c},ds}.setconfig(arg0, arg1, arg2, arg3, arg4, arg5);
  }
}


extern "C" {
  [[clang::import_name("action_data_size")]]
  uint32_t action_data_size();
  [[clang::import_name("read_action_data")]]
  uint32_t read_action_data(void*, uint32_t);
  __attribute__((weak))
  void __sysio_action_regoperator_epoch(unsigned long long r, unsigned long long c) {
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
    sysio::epoch{sysio::name{r},sysio::name{c},ds}.regoperator(arg0, arg1);
  }
}


extern "C" {
  [[clang::import_name("action_data_size")]]
  uint32_t action_data_size();
  [[clang::import_name("read_action_data")]]
  uint32_t read_action_data(void*, uint32_t);
  __attribute__((weak))
  void __sysio_action_unregoper_epoch(unsigned long long r, unsigned long long c) {
    size_t as = ::action_data_size();
    auto free_memory = [as](void* buf) { if (as >= 512) free(buf);};
    std::unique_ptr<void, decltype(free_memory)> buff{nullptr, free_memory};
    if (as > 0) {
      buff.reset(as >= 512 ? malloc(as) : alloca(as));
      ::read_action_data(buff.get(), as);
    }
    sysio::datastream<const char*> ds{(char*)buff.get(), as};
    sysio::name arg0; ds >> arg0;
    sysio::epoch{sysio::name{r},sysio::name{c},ds}.unregoper(arg0);
  }
}


extern "C" {
  [[clang::import_name("action_data_size")]]
  uint32_t action_data_size();
  [[clang::import_name("read_action_data")]]
  uint32_t read_action_data(void*, uint32_t);
  __attribute__((weak))
  void __sysio_action_advance_epoch(unsigned long long r, unsigned long long c) {
    size_t as = ::action_data_size();
    auto free_memory = [as](void* buf) { if (as >= 512) free(buf);};
    std::unique_ptr<void, decltype(free_memory)> buff{nullptr, free_memory};
    if (as > 0) {
      buff.reset(as >= 512 ? malloc(as) : alloca(as));
      ::read_action_data(buff.get(), as);
    }
    sysio::datastream<const char*> ds{(char*)buff.get(), as};
    sysio::epoch{sysio::name{r},sysio::name{c},ds}.advance();
  }
}


extern "C" {
  [[clang::import_name("action_data_size")]]
  uint32_t action_data_size();
  [[clang::import_name("read_action_data")]]
  uint32_t read_action_data(void*, uint32_t);
  __attribute__((weak))
  void __sysio_action_initgroups_epoch(unsigned long long r, unsigned long long c) {
    size_t as = ::action_data_size();
    auto free_memory = [as](void* buf) { if (as >= 512) free(buf);};
    std::unique_ptr<void, decltype(free_memory)> buff{nullptr, free_memory};
    if (as > 0) {
      buff.reset(as >= 512 ? malloc(as) : alloca(as));
      ::read_action_data(buff.get(), as);
    }
    sysio::datastream<const char*> ds{(char*)buff.get(), as};
    sysio::epoch{sysio::name{r},sysio::name{c},ds}.initgroups();
  }
}


extern "C" {
  [[clang::import_name("action_data_size")]]
  uint32_t action_data_size();
  [[clang::import_name("read_action_data")]]
  uint32_t read_action_data(void*, uint32_t);
  __attribute__((weak))
  void __sysio_action_replaceop_epoch(unsigned long long r, unsigned long long c) {
    size_t as = ::action_data_size();
    auto free_memory = [as](void* buf) { if (as >= 512) free(buf);};
    std::unique_ptr<void, decltype(free_memory)> buff{nullptr, free_memory};
    if (as > 0) {
      buff.reset(as >= 512 ? malloc(as) : alloca(as));
      ::read_action_data(buff.get(), as);
    }
    sysio::datastream<const char*> ds{(char*)buff.get(), as};
    sysio::name arg0; ds >> arg0;
    sysio::name arg1; ds >> arg1;
    sysio::epoch{sysio::name{r},sysio::name{c},ds}.replaceop(arg0, arg1);
  }
}


extern "C" {
  [[clang::import_name("action_data_size")]]
  uint32_t action_data_size();
  [[clang::import_name("read_action_data")]]
  uint32_t read_action_data(void*, uint32_t);
  __attribute__((weak))
  void __sysio_action_regoutpost_epoch(unsigned long long r, unsigned long long c) {
    size_t as = ::action_data_size();
    auto free_memory = [as](void* buf) { if (as >= 512) free(buf);};
    std::unique_ptr<void, decltype(free_memory)> buff{nullptr, free_memory};
    if (as > 0) {
      buff.reset(as >= 512 ? malloc(as) : alloca(as));
      ::read_action_data(buff.get(), as);
    }
    sysio::datastream<const char*> ds{(char*)buff.get(), as};
    uint8_t arg0; ds >> arg0;
    uint32_t arg1; ds >> arg1;
    sysio::epoch{sysio::name{r},sysio::name{c},ds}.regoutpost(arg0, arg1);
  }
}


extern "C" {
  [[clang::import_name("action_data_size")]]
  uint32_t action_data_size();
  [[clang::import_name("read_action_data")]]
  uint32_t read_action_data(void*, uint32_t);
  __attribute__((weak))
  void __sysio_action_pause_epoch(unsigned long long r, unsigned long long c) {
    size_t as = ::action_data_size();
    auto free_memory = [as](void* buf) { if (as >= 512) free(buf);};
    std::unique_ptr<void, decltype(free_memory)> buff{nullptr, free_memory};
    if (as > 0) {
      buff.reset(as >= 512 ? malloc(as) : alloca(as));
      ::read_action_data(buff.get(), as);
    }
    sysio::datastream<const char*> ds{(char*)buff.get(), as};
    sysio::epoch{sysio::name{r},sysio::name{c},ds}.pause();
  }
}


extern "C" {
  [[clang::import_name("action_data_size")]]
  uint32_t action_data_size();
  [[clang::import_name("read_action_data")]]
  uint32_t read_action_data(void*, uint32_t);
  __attribute__((weak))
  void __sysio_action_unpause_epoch(unsigned long long r, unsigned long long c) {
    size_t as = ::action_data_size();
    auto free_memory = [as](void* buf) { if (as >= 512) free(buf);};
    std::unique_ptr<void, decltype(free_memory)> buff{nullptr, free_memory};
    if (as > 0) {
      buff.reset(as >= 512 ? malloc(as) : alloca(as));
      ::read_action_data(buff.get(), as);
    }
    sysio::datastream<const char*> ds{(char*)buff.get(), as};
    sysio::epoch{sysio::name{r},sysio::name{c},ds}.unpause();
  }
}
