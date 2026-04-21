#pragma clang diagnostic ignored "-Weverything"
#include "/data/shared/code/wire/wire-sysio/contracts/sysio.chalg/src/sysio.chalg.cpp"
#include <sysio/datastream.hpp>
#include <sysio/name.hpp>


extern "C" {
  [[clang::import_name("action_data_size")]]
  uint32_t action_data_size();
  [[clang::import_name("read_action_data")]]
  uint32_t read_action_data(void*, uint32_t);
  __attribute__((weak))
  void __sysio_action_initchal_chalg(unsigned long long r, unsigned long long c) {
    size_t as = ::action_data_size();
    auto free_memory = [as](void* buf) { if (as >= 512) free(buf);};
    std::unique_ptr<void, decltype(free_memory)> buff{nullptr, free_memory};
    if (as > 0) {
      buff.reset(as >= 512 ? malloc(as) : alloca(as));
      ::read_action_data(buff.get(), as);
    }
    sysio::datastream<const char*> ds{(char*)buff.get(), as};
    uint64_t arg0; ds >> arg0;
    sysio::chalg{sysio::name{r},sysio::name{c},ds}.initchal(arg0);
  }
}


extern "C" {
  [[clang::import_name("action_data_size")]]
  uint32_t action_data_size();
  [[clang::import_name("read_action_data")]]
  uint32_t read_action_data(void*, uint32_t);
  __attribute__((weak))
  void __sysio_action_submitresp_chalg(unsigned long long r, unsigned long long c) {
    size_t as = ::action_data_size();
    auto free_memory = [as](void* buf) { if (as >= 512) free(buf);};
    std::unique_ptr<void, decltype(free_memory)> buff{nullptr, free_memory};
    if (as > 0) {
      buff.reset(as >= 512 ? malloc(as) : alloca(as));
      ::read_action_data(buff.get(), as);
    }
    sysio::datastream<const char*> ds{(char*)buff.get(), as};
    uint64_t arg0; ds >> arg0;
    sysio::checksum256 arg1; ds >> arg1;
    std::vector<sysio::name> arg2; ds >> arg2;
    std::vector<sysio::name> arg3; ds >> arg3;
    sysio::chalg{sysio::name{r},sysio::name{c},ds}.submitresp(arg0, arg1, arg2, arg3);
  }
}


extern "C" {
  [[clang::import_name("action_data_size")]]
  uint32_t action_data_size();
  [[clang::import_name("read_action_data")]]
  uint32_t read_action_data(void*, uint32_t);
  __attribute__((weak))
  void __sysio_action_escalate_chalg(unsigned long long r, unsigned long long c) {
    size_t as = ::action_data_size();
    auto free_memory = [as](void* buf) { if (as >= 512) free(buf);};
    std::unique_ptr<void, decltype(free_memory)> buff{nullptr, free_memory};
    if (as > 0) {
      buff.reset(as >= 512 ? malloc(as) : alloca(as));
      ::read_action_data(buff.get(), as);
    }
    sysio::datastream<const char*> ds{(char*)buff.get(), as};
    uint64_t arg0; ds >> arg0;
    sysio::chalg{sysio::name{r},sysio::name{c},ds}.escalate(arg0);
  }
}


extern "C" {
  [[clang::import_name("action_data_size")]]
  uint32_t action_data_size();
  [[clang::import_name("read_action_data")]]
  uint32_t read_action_data(void*, uint32_t);
  __attribute__((weak))
  void __sysio_action_submitres_chalg(unsigned long long r, unsigned long long c) {
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
    sysio::checksum256 arg4; ds >> arg4;
    sysio::chalg{sysio::name{r},sysio::name{c},ds}.submitres(arg0, arg1, arg2, arg3, arg4);
  }
}


extern "C" {
  [[clang::import_name("action_data_size")]]
  uint32_t action_data_size();
  [[clang::import_name("read_action_data")]]
  uint32_t read_action_data(void*, uint32_t);
  __attribute__((weak))
  void __sysio_action_enforce_chalg(unsigned long long r, unsigned long long c) {
    size_t as = ::action_data_size();
    auto free_memory = [as](void* buf) { if (as >= 512) free(buf);};
    std::unique_ptr<void, decltype(free_memory)> buff{nullptr, free_memory};
    if (as > 0) {
      buff.reset(as >= 512 ? malloc(as) : alloca(as));
      ::read_action_data(buff.get(), as);
    }
    sysio::datastream<const char*> ds{(char*)buff.get(), as};
    uint64_t arg0; ds >> arg0;
    sysio::chalg{sysio::name{r},sysio::name{c},ds}.enforce(arg0);
  }
}


extern "C" {
  [[clang::import_name("action_data_size")]]
  uint32_t action_data_size();
  [[clang::import_name("read_action_data")]]
  uint32_t read_action_data(void*, uint32_t);
  __attribute__((weak))
  void __sysio_action_slashop_chalg(unsigned long long r, unsigned long long c) {
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
    sysio::chalg{sysio::name{r},sysio::name{c},ds}.slashop(arg0, arg1);
  }
}
