#include <sysio/sysio.hpp>
#include <sysio/pb.hpp>
#include <test/test.pb.hpp>

namespace test {

class [[sysio::contract]] proto_abi_test : public sysio::contract {
public:
   using sysio::contract::contract;

   [[sysio::action]]
   sysio::pb<ActResult> hiproto(const sysio::pb<ActData>& msg) {
      sysio::check(static_cast<int32_t>(msg.id) == 1, "validate msg.id");
      sysio::check(static_cast<int32_t>(msg.type) == 2, "validate msg.type");
      sysio::check(msg.note == "hello", "validate msg.note");

      ActResult result;
      result.value = zpp::bits::vint32_t(42);
      result.str_value = "result_string";
      return result;
   }

   [[sysio::action]]
   void pbaction(const sysio::pb<ActData>& msg) {
      sysio::check(static_cast<int32_t>(msg.id) > 0, "id must be positive");
      sysio::print("Received protobuf action with id=", static_cast<int32_t>(msg.id));
   }
};

} // namespace test