#include <sysio.depot/sysio.depot.hpp>


using namespace fc::crypto;

void sysio::depot::echotest(const chain_kind_t& kind) {
   sysio::print("chain_kind_t = ", static_cast<uint8_t>(kind), "\n");
}
// This contract is intentionally left as a stub with no actions.
void sysio::depot::reservedelta(const chain_kind_t& kind, const asset& delta) {
}
void sysio::depot::swapquote(const chain_kind_t &source_chain,
                             const asset &source_amount,
                             const chain_kind_t &target_chain_kind) {

}
