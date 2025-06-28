#include <settlewns.hpp>
#include <sysio/print.hpp>
#include <sysio/crypto.hpp>
#include <sysio/print.hpp>
#include <cmath>
#include <string>

[[sysio::action]] void settlewns::initcontract()
{
}

[[sysio::action]] void settlewns::setstatevars(const uint8_t &pendingLimit)
{
}

// ----- Public Actions -----

/**
 * NOTES: The user will be submitting the intent to transfer (deposit) assets from ETH to WNS. And will HAVE to have gone through initial association process of Eth address to Wire account. Said Wire account will be the 'to' field in the transaction. Users can ONLY deposit 'to' themselves and are limited to 'pending_limit' number of transactions in a status of 0 (intent submitted).
 */
[[sysio::action]] uint64_t settlewns::initdeposit(const name &to, const wns::bytes &from, const token_value &assetParams)
{
    return 42;
};

/**
 * NOTES: The transaction signature submitted at this stage will be the transaction signature of the EVM Deposit call made after initdeposit along with block number, transaction id, and trx status. The transaction id is the transaction hash of the EVM Deposit call. The block number is the block number of the EVM Deposit call. The trx status will be set to 1 (pending) and is no longer able to be cancelled at this point.
*/
[[sysio::action]] void settlewns::setpending(const uint64_t &key, const wns::bytes &trxId, const uint128_t &blockNum, const sysio::signature &sig, const checksum256 &depRoot) {
};

[[sysio::action]] void settlewns::canceldep(const uint64_t &key) {
};

[[sysio::action]] void settlewns::depstate(const uint64_t &key, const uint8_t &status)
{
}

[[sysio::action]] uint64_t settlewns::createbatch(const name &batchOperator, const asset &fee, const time_point_sec &tof, const uint16_t &minParticipants, const uint8_t &maxParticipants)
{
   return 43;
}

[[sysio::action]] void settlewns::lockbatch(const uint64_t batch)
{
}

[[sysio::action]] void settlewns::batchw(const uint64_t batch, const vector<wns::Withdrawal>& withdrawals)
{
}

[[sysio::action]] void settlewns::claimbatch(const uint64_t batch, const wns::bytes &trxId, const uint128_t &blockNum, const sysio::signature &sig, const checksum256 &msgDigest) {
}

[[sysio::action]] void settlewns::renewbatch(const uint64_t &batch, const time_point_sec &tof) {
}

[[sysio::action]] void settlewns::cancelbatch(const uint64_t batch)
{
}

[[sysio::action]] void settlewns::batchstate(const uint64_t batch, uint8_t state)
{
}

[[sysio::action]] vector<uint64_t> settlewns::withdraw(const vector<uint64_t>& utxos, const name& user, const vector<uint64_t>& batches)
{
    vector<uint64_t> withdrawal_ids;
    return withdrawal_ids;
}

[[sysio::action]] void settlewns::joinbatches(const uint64_t& withdrawal, const vector<uint64_t>& batches) {
}

[[sysio::action]] void settlewns::exitbatch(const uint64_t& withdrawal, const uint64_t& batch) {
}

[[sysio::action]] void settlewns::cancelwithd(const uint64_t& withdrawal) {
}

[[sysio::action]] pair<uint64_t,vector<uint64_t>> settlewns::selfwithd(const name &user, const vector<uint64_t>& utxos) {
   return std::make_pair(22, vector<uint64_t>());
}

[[sysio::action]] void settlewns::snoop()
{
    print("we snoopin");
}

[[sysio::action]] void settlewns::blackliste(const wns::bytes &ethAddress)
{
}

[[sysio::action]] void settlewns::blacklistw(const name &wireAddress ) {
}
