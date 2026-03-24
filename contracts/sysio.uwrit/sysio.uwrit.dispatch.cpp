#include <cstdint>
#include <sysio/name.hpp>
extern "C" {
  __attribute__((import_name("sysio_assert_code"))) void sysio_assert_code(uint32_t, uint64_t);  void sysio_set_contract_name(uint64_t n);
  void __sysio_action_confirmuw_uwrit(uint64_t r, uint64_t c);
  void __sysio_action_distfee_uwrit(uint64_t r, uint64_t c);
  void __sysio_action_expirelock_uwrit(uint64_t r, uint64_t c);
  void __sysio_action_setconfig_uwrit(uint64_t r, uint64_t c);
  void __sysio_action_slash_uwrit(uint64_t r, uint64_t c);
  void __sysio_action_submituw_uwrit(uint64_t r, uint64_t c);
  void __sysio_action_updcltrl_uwrit(uint64_t r, uint64_t c);
  __attribute__((export_name("apply"), visibility("default")))
  void apply(uint64_t r, uint64_t c, uint64_t a) {
    sysio_set_contract_name(r);
    if (c == r) {
      switch (a) {
      case "confirmuw"_n.value:
        __sysio_action_confirmuw_uwrit(r, c);
        break;
      case "distfee"_n.value:
        __sysio_action_distfee_uwrit(r, c);
        break;
      case "expirelock"_n.value:
        __sysio_action_expirelock_uwrit(r, c);
        break;
      case "setconfig"_n.value:
        __sysio_action_setconfig_uwrit(r, c);
        break;
      case "slash"_n.value:
        __sysio_action_slash_uwrit(r, c);
        break;
      case "submituw"_n.value:
        __sysio_action_submituw_uwrit(r, c);
        break;
      case "updcltrl"_n.value:
        __sysio_action_updcltrl_uwrit(r, c);
        break;
      default:
        if ( r != "sysio"_n.value) sysio_assert_code(false, 1);
      }
    } else {
    }
  }
}
