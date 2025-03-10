#pragma once

#include <sysio/sysio.hpp>
#include <sysio/system.hpp> // For current_block_number
#include <sysio/singleton.hpp>
#include <sysio/asset.hpp>
#include <sysio/dispatcher.hpp> // For SYSIO_DISPATCH of native action
#include <sysio/privileged.hpp> 
#include <sysio/string.hpp> // Need for is_sysio_account?

/**
 *  ---- General TODOs ----
 * 
 * TODO: Move network_gen to a global table as a single point of truth.
 */
namespace sysio {
    class [[sysio::contract("sysio.roa")]] roa : public contract {
        public:
            using contract::contract;

            /** 
             * @brief Initializes sysio.roa, should be called as last step in Bios Boot Sequence, activating the ROA resource management system.
             * 
             * @param total_sys The total starting SYS of the network.
             * @param ram_byte_price The amount of bytes .0001 SYS is worth, set in roastate table. If SYS precision is different, same concept applies, the single smallest unit of the core token.
             */
            [[sysio::action]]
            void activateroa(const asset& total_sys, const uint64_t& bytes_per_unit);

            /**
             * TODO: Ideally make this on-notify or some automated system on network expansion. Will address this down the line.
             * 
             * @brief Updates the amount of bytes .0001 SYS allocates. Requires Node Owner multisig approval, should only be used on network expansion.
             * 
             * @param bytes_per_unit The NEW amount of bytes .0001 SYS is worth.
             */
            [[sysio::action]]
            void setbyteprice(const uint64_t& bytes_per_unit);

            /**
             * TODO: Convert to multi step process. Restrict auth to Node Operator accounts.
             * TODO: Notify council contract on registration, think about order of operations on council contract existing.
             * 
             * @brief Registers 'owner' as a Node Owner scoped by network_gen, granting SYS allotment based on Tier and creates a default policy for owner.
             * 
             * NOTE: Can only register for the current generation of the network.
             * 
             * @param owner The account name of the Node Owner.
             * @param tier  The tier of Node they are an owner of. 1, 2, 3
             */
            [[sysio::action]]
            void regnodeowner(const name& owner, const uint8_t& tier);

            /**
             * @brief Adds a row to the policies table scoped to 'issuer' ( Node Owner ) and either creates a row in 'reslimit' for 'owner' or increments the values if 'owner' already has a row.
             * 
             * NOTE: Cannot allocate CPU / NET to system accounts to maintain infinite CPU / NET on these accounts.
             * 
             * @param owner The account to issue this policy for.
             * @param issuer The Node Owner issuing the policy.
             * @param net_weight The amount of SYS allocated for NET
             * @param cpu_weight The amount of SYS allocated for CPU
             * @param ram_weight The amount of SYS allocated for RAM.
             * @param time_block A block number, the policy can't be reclaimed or reduced before this block.
             * @param network_gen Generation of issuer, in cases were you are a Node Owner in multiple, specifies which allocation of SYS to pull from.
             */
            [[sysio::action]]
            void addpolicy(const name& owner, const name& issuer, const asset& net_weight, const asset& cpu_weight, const asset& ram_weight, const uint32_t& time_block, const uint8_t& network_gen);

            /** 
             * @brief Increase the resource limits on an existing policy. Adds new weights, to existing policy values.
             * 
             * NOTE: Cannot allocate CPU / NET to system accounts to maintain infinite CPU / NET on these accounts.
             * 
             * @param owner The account this policy is issued to.
             * @param issuer The Node Owner who issued this policy.
             * @param net_weight The amount in SYS to increase NET by.
             * @param cpu_weight The amount in SYS to increase CPU by.
             * @param ram_weight The amount in SYS to increase RAM by.
             * @param network_gen Generation of issuer, in cases were you are a Node Owner in multiple, specifies which allocation of SYS to pull from.
             */
            [[sysio::action]]
            void expandpolicy(const name& owner, const name& issuer, const asset& net_weight, const asset& cpu_weight, const asset& ram_weight, const uint8_t& network_gen);
            
            /**
             * @brief Increases the policie's time_block extending the policies term.
             * 
             * @param owner The account this policy is issued to.
             * @param issuer The Node Owner who issued this policy.
             * @param new_time_block The new block number this policy can't be reduced or reclaimed till.
             */
            [[sysio::action]]
            void extendpolicy(const name& owner, const name& issuer, const uint32_t& new_time_block);

            /**
             * @brief Decrease the resource limits on an existing policy. Subtracts new weights from existing values. Only callable after policie's time_block.
             * 
             * Note: Will reclaim UPTO ram_weight worth of bytes, limited to the pool of unused bytes on 'owner's reslimit and upper bound by the policy ram_weight.
             * 
             * @param owner The account this policy is issued to.
             * @param issuer The Node Owner who issued this policy.
             * @param net_weight The amount in SYS to decrease NET by.
             * @param cpu_weight The amount in SYS to decrease CPU by.
             * @param ram_weight The amount in SYS to attempt decreasing RAM by, returning only
             * @param network_gen Generation of issuer, in cases were you are a Node Owner in multiple, specifies which allocation of SYS to adjust. 
             */
            [[sysio::action]]
            void reducepolicy(const name& owner, const name& issuer, const asset& net_weight, const asset& cpu_weight, const asset& ram_weight, const uint8_t& network_gen);

        private:
            
            /**
             * Config variables for ROA.
             */
            struct [[sysio::table("roastate")]] roa_state {
                bool is_active = false;
                asset total_sys = asset(0, symbol("SYS", 4));
                uint64_t bytes_per_unit = 0;
                uint8_t network_gen = 0; // Network Generation

                SYSLIB_SERIALIZE(roa_state, (is_active)(total_sys)(bytes_per_unit)(network_gen))
            };

            typedef sysio::singleton<"roastate"_n, roa_state> roastate_t;

            /**
             * Scoped to network_gen
             * 
             * Basic table tracking who T1-3 Node Owners are and their availble vs allocated SYS.
             */
            struct [[sysio::table]] nodeowners {
                name owner;          // Node Owners account name.
                uint8_t tier;        // Represents what tier they hold: 1, 2, or 3
                asset total_sys;     // Total SYS alloted based on tier.
                asset allocated_sys; // Total SYS allocated via policies they issued.
                asset allocated_bw;  // Total SYS allocated to CPU / NET.
                asset allocated_ram; // Total SYS allocated to RAM.
                uint8_t network_gen; // The generation this Node Owner was registered for.

                uint64_t primary_key() const { return owner.value; }
                uint64_t by_tier() const { return static_cast<uint64_t>(tier); }
            };

            typedef multi_index<"nodeowners"_n, nodeowners, 
                indexed_by<"bytier"_n, const_mem_fun<nodeowners, uint64_t, &nodeowners::by_tier>>
            > nodeowners_t;

            /**
             * This table is scoped to Node Owner's account names and is used to track all policies issued by Node Owners.
             */
            struct [[sysio::table]] policies {
                name owner;                 // Account name this policy applies to.
                name issuer;                // Account name of the Node Owner who issued this policy.
                asset net_weight;           // The amount of SYS allocated for NET.
                asset cpu_weight;           // The amount of SYS allocated for CPU.
                asset ram_weight;           // The amount of SYS allocated for RAM.
                uint64_t bytes_per_unit;    // The amount of bytes .0001 SYS was worth when the policy was created.
                uint32_t time_block;        // Block number, this policy can't be deleted or have its values lowered before the networks current block num >= time_block.

                uint64_t primary_key() const { return owner.value; }
            };

            typedef multi_index<"policies"_n, policies> policies_t;

            /**
             * Scoped to Owner: Holds upper limits of resources an account has access to. This table is used by the Node Operators to maintain usage metrics, replaces 'userres' on sysio.
             */
            struct [[sysio::table]] reslimit {
                name owner;            // Account name this policy applies to
                asset net_weight;      // Total NET allocated
                asset cpu_weight;      // Total CPU allocated
                uint64_t ram_bytes;    // Total RAM allocated

                uint64_t primary_key() const { return owner.value; }
            };

            typedef sysio::multi_index<"reslimit"_n, reslimit> reslimit_t;

            // ---- Private Functions ----

            /**
             * @brief A simple getter for totall allotted SYS based on tier number: 1, 2, 3. Matches rounding and logic used in activation.
             * 
             * @return An asset containing the amount of SYS this tier gets
             */
            asset get_allocation_for_tier(uint8_t tier);

            bool is_sysio_account(const name& account) {
                std::string acc_str = account.to_string();
                if (acc_str == "sysio") return true;
                if (acc_str.size() > 5 && acc_str.rfind("sysio.", 0) == 0) return true;
                return false;
            }
    }; // namespace roa
} // namespace sysio
