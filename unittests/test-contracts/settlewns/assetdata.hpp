#pragma once

#include <sysio/sysio.hpp>
#include <sysio/system.hpp>
#include <sysio/singleton.hpp>
#include <sysio/asset.hpp>
#include <sysio/crypto.hpp>
#include <stdlib.h>
#include <cstdlib>
#include <string>
#include <vector>
#include <cstdint>
#include <uint256.hpp>

using namespace sysio;
using namespace std;

namespace wns
{
    constexpr uint8_t BATCH_OPEN = 0;
    constexpr uint8_t BATCH_LOCKED = 1;
    constexpr uint8_t BATCH_CANCELLED = 2;
    constexpr uint8_t BATCH_EXECUTED = 3;
    constexpr uint8_t BATCH_CLAIMED = 4;
    constexpr uint8_t BATCH_REJECTED = 5;
    constexpr uint8_t BATCH_DONE = 6;

    constexpr uint8_t DEPOSIT_INTENT = 0;
    constexpr uint8_t DEPOSIT_APPROVED = 1;
    constexpr uint8_t DEPOSIT_PENDING = 2;
    constexpr uint8_t DEPOSIT_CONFIRMED = 3;
    constexpr uint8_t DEPOSIT_REJECTED = 4;
    constexpr uint8_t DEPOSIT_CANCELLED = 5;

    // Asset type mapping: Future types will be added here.
    enum AssetType : uint64_t
    {
        NONE = 0,
        ERC20 = 20,
        ERC721 = 721,
        ERC1155 = 1155
    };

    typedef std::vector<char> bytes;
    class token_value {
        public:
        // Fields
        bytes contractAddress = bytes(20,0); // 20 bytes
        uint64_t tokenType = AssetType::NONE;
        uint256_t tokenId = 0;
        uint256_t amount = 0;

        token_value operator+(const token_value& other) const {
            token_type_match(other);
            return token_value{contractAddress, tokenType, tokenId, amount + other.amount};
        }


        token_value operator-(const token_value& other) const {
            token_type_match(other);
            check(amount >= other.amount, "Subtraction would result in a negative amount");
            return token_value{contractAddress, tokenType, tokenId, amount - other.amount};
        }

        token_value& operator+=(const token_value& other) {
            token_type_match(other);
            amount += other.amount;
            return *this;
        }

        token_value& operator-=(const token_value& other) {
            token_type_match(other);
            check(amount >= other.amount, "Subtraction would result in a negative amount");
            amount -= other.amount;
            return *this;
        }

        // Comparison operators
        bool operator==(const token_value& other) const {
            return contractAddress == other.contractAddress &&
                tokenType == other.tokenType &&
                tokenId == other.tokenId &&
                amount == other.amount;
        }

        // Utility function for field validation
        void token_type_match(const token_value& other) const {
            check(contractAddress == other.contractAddress, "Contract addresses do not match");
            check(tokenType == other.tokenType, "Token types do not match");
            check(tokenId == other.tokenId, "Token IDs do not match");
        }
    };
    typedef unsigned __int128 uint128_t;

    // TYPES
    struct Withdrawal {
        token_value value;
        wns::bytes to;              // 20 bytes, address of the receiver

        SYSLIB_SERIALIZE(Withdrawal, (value)(to))
    };

    struct Deposit {
        token_value value;
        name to;
        wns::bytes from;
    };

    struct Transfer {
        token_value value;
        name to;
        name from;
    };
    struct ERC20Balance
    {
        uint256_t balance;

        SYSLIB_SERIALIZE(ERC20Balance, (balance))
    };

    struct ERC721Balance
    {
        vector<uint256_t> tokenIds;

        SYSLIB_SERIALIZE(ERC721Balance, (tokenIds))
    };

    struct ERC1155Balance
    {
        // Need a mapping of tokenIds to amounts
        map<uint256_t, uint256_t> tokenIdsToAmounts;

        SYSLIB_SERIALIZE(ERC1155Balance, (tokenIdsToAmounts))
    };
    typedef ERC20Balance erc20balance;
    typedef ERC721Balance erc721balance;
    typedef ERC1155Balance erc1155balance;

    typedef std::variant<erc20balance, erc721balance, erc1155balance> AssetBalance;
}