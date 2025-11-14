#include <sysio.depot/sysio.depot.hpp>



void sysio::depot::echotest(const chain_kind& kind) {
   sysio::print("chain_kind = ", static_cast<uint8_t>(kind), "\n");
}
// This contract is intentionally left as a stub with no actions.
void sysio::depot::reservedelta(const chain_kind& kind, const asset& delta) {
}
void sysio::depot::swapquote(const uint8_t &source_chain,
                             const asset &source_amount,
                             const uint8_t &target_chain_kind) {

}
