#include <sysio/chain/wasm_sysio_validation.hpp>

#include <boost/mpl/joint_view.hpp>
#include <boost/mpl/list.hpp>
#include <boost/test/unit_test.hpp>

#include <limits>

using namespace sysio::chain;

namespace {

/// Every opcode whose concrete specialization is passed to the legacy offset validator.
using memory_instructions = boost::mpl::joint_view<
   boost::mpl::list<
      wasm_validations::op_constrainers::i32_load_t, wasm_validations::op_constrainers::i64_load_t,
      wasm_validations::op_constrainers::f32_load_t, wasm_validations::op_constrainers::f64_load_t,
      wasm_validations::op_constrainers::i32_load8_s_t, wasm_validations::op_constrainers::i32_load8_u_t,
      wasm_validations::op_constrainers::i32_load16_s_t, wasm_validations::op_constrainers::i32_load16_u_t,
      wasm_validations::op_constrainers::i64_load8_s_t, wasm_validations::op_constrainers::i64_load8_u_t,
      wasm_validations::op_constrainers::i64_load16_s_t, wasm_validations::op_constrainers::i64_load16_u_t,
      wasm_validations::op_constrainers::i64_load32_s_t, wasm_validations::op_constrainers::i64_load32_u_t>,
   boost::mpl::list<wasm_validations::op_constrainers::i32_store_t, wasm_validations::op_constrainers::i64_store_t,
                    wasm_validations::op_constrainers::f32_store_t, wasm_validations::op_constrainers::f64_store_t,
                    wasm_validations::op_constrainers::i32_store8_t, wasm_validations::op_constrainers::i32_store16_t,
                    wasm_validations::op_constrainers::i64_store8_t, wasm_validations::op_constrainers::i64_store16_t,
                    wasm_validations::op_constrainers::i64_store32_t>>;

} // namespace

BOOST_AUTO_TEST_SUITE(wasm_validator_tests)

/// Check both sides of the offset boundary through instr's virtual dispatch for every memory opcode.
BOOST_AUTO_TEST_CASE_TEMPLATE(memory_offset_boundaries, Instruction, memory_instructions) {
   Instruction instruction{};
   wasm_ops::instr& dispatched = instruction;
   instruction.field = {0, 0};
   BOOST_CHECK_NO_THROW(dispatched.visit({}));
   instruction.field.o = wasm_constraints::maximum_linear_memory - 1;
   BOOST_CHECK_NO_THROW(dispatched.visit({}));
   instruction.field.o = wasm_constraints::maximum_linear_memory;
   BOOST_CHECK_THROW(dispatched.visit({}), wasm_execution_error);
   instruction.field.o = std::numeric_limits<uint32_t>::max();
   BOOST_CHECK_THROW(dispatched.visit({}), wasm_execution_error);
}

BOOST_AUTO_TEST_SUITE_END()
