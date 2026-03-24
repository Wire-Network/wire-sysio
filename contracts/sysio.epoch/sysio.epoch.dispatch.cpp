#include <cstdint>
#include <sysio/name.hpp>
extern "C" {
  __attribute__((import_name("sysio_assert_code"))) void sysio_assert_code(uint32_t, uint64_t);  void sysio_set_contract_name(uint64_t n);
  void __sysio_action_advance_epoch(uint64_t r, uint64_t c);
  void __sysio_action_initgroups_epoch(uint64_t r, uint64_t c);
  void __sysio_action_pause_epoch(uint64_t r, uint64_t c);
  void __sysio_action_regoperator_epoch(uint64_t r, uint64_t c);
  void __sysio_action_regoutpost_epoch(uint64_t r, uint64_t c);
  void __sysio_action_replaceop_epoch(uint64_t r, uint64_t c);
  void __sysio_action_setconfig_epoch(uint64_t r, uint64_t c);
  void __sysio_action_unpause_epoch(uint64_t r, uint64_t c);
  void __sysio_action_unregoper_epoch(uint64_t r, uint64_t c);
  __attribute__((export_name("apply"), visibility("default")))
  void apply(uint64_t r, uint64_t c, uint64_t a) {
    sysio_set_contract_name(r);
    if (c == r) {
      switch (a) {
      case "advance"_n.value:
        __sysio_action_advance_epoch(r, c);
        break;
      case "initgroups"_n.value:
        __sysio_action_initgroups_epoch(r, c);
        break;
      case "pause"_n.value:
        __sysio_action_pause_epoch(r, c);
        break;
      case "regoperator"_n.value:
        __sysio_action_regoperator_epoch(r, c);
        break;
      case "regoutpost"_n.value:
        __sysio_action_regoutpost_epoch(r, c);
        break;
      case "replaceop"_n.value:
        __sysio_action_replaceop_epoch(r, c);
        break;
      case "setconfig"_n.value:
        __sysio_action_setconfig_epoch(r, c);
        break;
      case "unpause"_n.value:
        __sysio_action_unpause_epoch(r, c);
        break;
      case "unregoper"_n.value:
        __sysio_action_unregoper_epoch(r, c);
        break;
      default:
        if ( r != "sysio"_n.value) sysio_assert_code(false, 1);
      }
    } else {
    }
  }
}
