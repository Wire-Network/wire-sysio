#include <cstdint>
#include <sysio/name.hpp>
extern "C" {
  __attribute__((import_name("sysio_assert_code"))) void sysio_assert_code(uint32_t, uint64_t);  void sysio_set_contract_name(uint64_t n);
  void __sysio_action_buildenv_msgch(uint64_t r, uint64_t c);
  void __sysio_action_crank_msgch(uint64_t r, uint64_t c);
  void __sysio_action_createreq_msgch(uint64_t r, uint64_t c);
  void __sysio_action_deliver_msgch(uint64_t r, uint64_t c);
  void __sysio_action_evalcons_msgch(uint64_t r, uint64_t c);
  void __sysio_action_processmsg_msgch(uint64_t r, uint64_t c);
  void __sysio_action_queueout_msgch(uint64_t r, uint64_t c);
  __attribute__((export_name("apply"), visibility("default")))
  void apply(uint64_t r, uint64_t c, uint64_t a) {
    sysio_set_contract_name(r);
    if (c == r) {
      switch (a) {
      case "buildenv"_n.value:
        __sysio_action_buildenv_msgch(r, c);
        break;
      case "crank"_n.value:
        __sysio_action_crank_msgch(r, c);
        break;
      case "createreq"_n.value:
        __sysio_action_createreq_msgch(r, c);
        break;
      case "deliver"_n.value:
        __sysio_action_deliver_msgch(r, c);
        break;
      case "evalcons"_n.value:
        __sysio_action_evalcons_msgch(r, c);
        break;
      case "processmsg"_n.value:
        __sysio_action_processmsg_msgch(r, c);
        break;
      case "queueout"_n.value:
        __sysio_action_queueout_msgch(r, c);
        break;
      default:
        if ( r != "sysio"_n.value) sysio_assert_code(false, 1);
      }
    } else {
    }
  }
}
