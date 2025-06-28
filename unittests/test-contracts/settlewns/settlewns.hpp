#pragma once

#include <assetdata.hpp>
#include <sysio/system.hpp>
#include <sysio/singleton.hpp>
#include <sysio/asset.hpp>
#include <sysio/crypto.hpp>


using namespace sysio;
using namespace wns;


class [[sysio::contract("settlewns")]] settlewns : public contract { 
public:
    using contract::contract;

    settlewns(name receiver, name code, datastream<const char*> ds)
    : contract(receiver, code, ds)
    {
    };

        /**
         * @brief Initializes the contract with default values. Setting our state table.
         */
        [[sysio::action]] void initcontract();

        /**
         * @brief Updates the state table with the new values.
         *
         * @param pending_limit The new maximum number of pending transactions allowed per user.
         */
        [[sysio::action]] void setstatevars(const uint8_t &pendingLimit);

        /**
         * @brief Initializes the intent to deposit tokens into the WNS Ecosystem. This will create a new row in the 'deposit' table with the transaction status INTENT (0). With a limited number of trx status 0 transactions allowed per user, based on pending_limit in state table. Transactions in status INTENT (0) will be cancellable by the user.
         *
         * @param to The WIRE account that is receiving the tokens.
         * @param from Ethereum address of the user.
         * @param assetParams The token value object containing the contract address, token type, token ID, and amount.
         */
        [[sysio::action]] uint64_t initdeposit(const name &to, const wns::bytes &from, const token_value &assetParams);

        /**
         * @brief Called by user after the transaction has been created on the Ethereum side. This will update the transaction status to PENDING (2). As well as updating the transaction log with ID and block number of the Eth transaction. Your deposit is now ready to be confirmed by a validator.
         *
         * @param key The primary key of the initiated trx row in 'deposit' table.
         * @param trxId Transaction ID of the transaction on Ethereum.
         * @param blockNum Block number the transaction is included in on Ethereum.
         * @param sig The new signature of the actual ETH deposit transaction K1 formatted.
         * @param depRoot The deposit root of the Ethereum transaction.
         */
        [[sysio::action]] void setpending(const uint64_t &key, const wns::bytes &trxId, const uint128_t &blockNum, const sysio::signature &sig, const checksum256 &depRoot);

        /**
         * @brief Cancels a pending transaction. Only callable by the user who initiated the transaction.
         *
         * @param key ID of pending deposit.
         * @return void
         */
        [[sysio::action]] void canceldep(const uint64_t &key);

        /**
         * @brief Final step in deposit flow, called by approved validators. After the deposit is validated status gets set to APPROVED, CONFIRMED, or REJECTED. If CONFIRMED the deposit is added to the user's balance.
         *
         * @param key ID of deposit
         * @param status New status determined by validator; must be APPROVED (1), CONFIRMED (3), or REJECTED (4)
         */
        [[sysio::action]] void depstate(const uint64_t &key, const uint8_t &status);

        /**
         * @brief renew time of flight for empty batch withdraw
         * 
         * @param batch Batch ID
         * @param tof New time of flight for batch
         */
        [[sysio::action]] void renewbatch(const uint64_t &batch, const time_point_sec &tof);

        /**
         * @brief Create a batch facilitating the Transfer of tokens from one user to a set of users, updating balances and creating UTXOs
         *
         * @param batchOperator The account that is executing the batch
         * @param fee The fee for the batch
         * @param tof The time of execution
         * @param minParticipants The minimum number of participants required
         * @param maxParticipants The maximum number of participants allowed
         */
        [[sysio::action]] uint64_t createbatch(const name &batchOperator, const asset &fee, const time_point_sec &tof, const uint16_t &minParticipants, const uint8_t &maxParticipants);

        /**
         * @brief 
         * 
         * @param batch 
         */
        [[sysio::action]] void lockbatch(const uint64_t batch);

        /**
         * @brief 
         * 
         * @param batch 
         * @param withdrawals
         */
        [[sysio::action]] void batchw(const uint64_t batch, const vector<wns::Withdrawal>& withdrawals);

        /**
         * @brief 
         * 
         * @param batch 
         * @param trx_id 
         * @param blockNum 
         * @param sig 
         * @param msgDigest 
         */
        [[sysio::action]] void claimbatch(const uint64_t batch, const wns::bytes &trxId, const uint128_t &blockNum, const sysio::signature &sig, const checksum256 &msgDigest);

        /**
         * @brief 
         * 
         * @param batch 
         */
        [[sysio::action]] void cancelbatch(const uint64_t batch);
        
        /**
         * @brief 
         * 
         * @param batch 
         * @param state 
         */
        [[sysio::action]] void batchstate(const uint64_t batch, uint8_t state);

        /**
         * @brief 
         * 
         * @param utxos 
         * @param user 
         * @param batch 
         */
        [[sysio::action]] vector<uint64_t> withdraw(const vector<uint64_t>& utxos, const name& user, const vector<uint64_t>& batch);

        /**
         * @brief 
         * 
         * @param withdrawal 
         * @param batches 
         */
        [[sysio::action]] void joinbatches(const uint64_t& withdrawal, const vector<uint64_t>& batches);

        /**
         * @brief 
         * 
         * @param withdrawal 
         * @param batch 
         */
        [[sysio::action]] void exitbatch(const uint64_t& withdrawal, const uint64_t& batch);

        /**
         * @brief 
         * 
         * @param withdrawal 
         */
        [[sysio::action]] void cancelwithd(const uint64_t& withdrawal);

        /**
         * @brief 
         * 
         * @param user
         * @param utxos
         */
        [[sysio::action]] pair<uint64_t,vector<uint64_t>> selfwithd(const name &user, const vector<uint64_t>& utxos);

        /**
         * @brief 
         */
        [[sysio::action]] void snoop();

        /**
         * @brief Adds / Removes an Eth Address from the blacklist. Blacklisted users will not be able to use any actions.
         *
         * @param ethAddress The Ethereum address of the user to blacklist.
         * @return void
         */
        [[sysio::action]] void blackliste(const wns::bytes& ethAddress);

        /**
         * @brief Adds / Removes a Wire username from the blacklist. Blacklisted users will not be able to use any actions.
         *
         * @param wireAddress The Ethereum address of the user to blacklist.
         * @return void
         */
        [[sysio::action]] void blacklistw(const name& wireAddress);

    private:
};

