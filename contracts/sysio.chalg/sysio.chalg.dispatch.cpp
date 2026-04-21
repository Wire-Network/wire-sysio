#include <cstdint>
#include <sysio/name.hpp>
extern "C" {
  __attribute__((import_name("sysio_assert_code"))) void sysio_assert_code(uint32_t, uint64_t);  void sysio_set_contract_name(uint64_t n);
  void __sysio_action_enforce_chalg(uint64_t r, uint64_t c);
  void __sysio_action_escalate_chalg(uint64_t r, uint64_t c);
  void __sysio_action_initchal_chalg(uint64_t r, uint64_t c);
  void __sysio_action_slashop_chalg(uint64_t r, uint64_t c);
  void __sysio_action_submitres_chalg(uint64_t r, uint64_t c);
  void __sysio_action_submitresp_chalg(uint64_t r, uint64_t c);
  __attribute__((export_name("apply"), visibility("default")))
  void apply(uint64_t r, uint64_t c, uint64_t a) {
    sysio_set_contract_name(r);
    if (c == r) {
      switch (a) {
      case "enforce"_n.value:
        __sysio_action_enforce_chalg(r, c);
        break;
      case "escalate"_n.value:
        __sysio_action_escalate_chalg(r, c);
        break;
      case "initchal"_n.value:
        __sysio_action_initchal_chalg(r, c);
        break;
      case "slashop"_n.value:
        __sysio_action_slashop_chalg(r, c);
        break;
      case "submitres"_n.value:
        __sysio_action_submitres_chalg(r, c);
        break;
      case "submitresp"_n.value:
        __sysio_action_submitresp_chalg(r, c);
        break;
      default:
        if ( r != "sysio"_n.value) sysio_assert_code(false, 1);
      }
    } else {
    }
  }
}
