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

            typedef std::vector<char> bytes;

            /** 
             * @brief Initializes sysio.roa, should be called as last step in Bios Boot Sequence, activating the ROA resource management system.
             * 
             * @param totalSys The total starting SYS of the network.
             * @param bytesPerUnit The amount of bytes .0001 SYS is worth, set in roastate table. If SYS precision is different, same concept applies, the single smallest unit of the core token.
             */
            [[sysio::action]]
            void activateroa(const asset& totalSys, const uint64_t& bytesPerUnit);

            /**
             * TODO: Ideally make this on-notify or some automated system on network expansion. Will address this down the line.
             * 
             * @brief Updates the amount of bytes .0001 SYS allocates. Requires Node Owner multisig approval, should only be used on network expansion.
             * 
             * @param bytesPerUnit The NEW amount of bytes .0001 SYS is worth.
             */
            [[sysio::action]]
            void setbyteprice(const uint64_t& bytesPerUnit);



            /**
             * @brief Adds a row to the policies table scoped to 'issuer' ( Node Owner ) and either creates a row in 'reslimit' for 'owner' or increments the values if 'owner' already has a row.
             * 
             * NOTE: Cannot allocate CPU / NET to system accounts to maintain infinite CPU / NET on these accounts.
             * 
             * @param owner The account to issue this policy for.
             * @param issuer The Node Owner issuing the policy.
             * @param netWeight The amount of SYS allocated for NET
             * @param cpuWeight The amount of SYS allocated for CPU
             * @param ramWeight The amount of SYS allocated for RAM.
             * @param timeBlock A block number, the policy can't be reclaimed or reduced before this block.
             * @param networkGen Generation of issuer, in cases were you are a Node Owner in multiple, specifies which allocation of SYS to pull from.
             */
            [[sysio::action]]
            void addpolicy(const name& owner, const name& issuer, const asset& netWeight, const asset& cpuWeight, const asset& ramWeight, const uint32_t& timeBlock, const uint8_t& networkGen);

            /** 
             * @brief Increase the resource limits on an existing policy. Adds new weights, to existing policy values.
             * 
             * NOTE: Cannot allocate CPU / NET to system accounts to maintain infinite CPU / NET on these accounts.
             * 
             * @param owner The account this policy is issued to.
             * @param issuer The Node Owner who issued this policy.
             * @param netWeight The amount in SYS to increase NET by.
             * @param cpuWeight The amount in SYS to increase CPU by.
             * @param ramWeight The amount in SYS to increase RAM by.
             * @param networkGen Generation of issuer, in cases were you are a Node Owner in multiple, specifies which allocation of SYS to pull from.
             */
            [[sysio::action]]
            void expandpolicy(const name& owner, const name& issuer, const asset& netWeight, const asset& cpuWeight, const asset& ramWeight, const uint8_t& networkGen);
            
            /**
             * @brief Increases the policie's time_block extending the policies term.
             * 
             * @param owner The account this policy is issued to.
             * @param issuer The Node Owner who issued this policy.
             * @param newTimeBlock The new block number this policy can't be reduced or reclaimed till.
             */
            [[sysio::action]]
            void extendpolicy(const name& owner, const name& issuer, const uint32_t& newTimeBlock);

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


            /**
             * @brief Initiates the node registration process.
             * 
             * @param owner The account name of the node owner.
             * @param tier Tier level of the node owner.
             */
            [[sysio::action]]
            void initnodereg(const name& owner);


            /**
             * @brief After the Node Owner deposits their Wire-Node NFT to the NodeMan contract on Ethereum this action is called to update their intent with the transaction ID and Signature of the Eth Deposit setting the status of their registration to pending (1)
             * 
             * @param owner The account name of the node owner.
             * @param tier The tier level of the node owner.
             * @param trxId The transaction ID of the transaction on Ethereum. 
             * @param blockNum The block number on ETH that the deposit was in.
             * @param sig The signature of the Ethereum deposit
             */
            [[sysio::action]]
            void setpending(const name& owner, const uint8_t& tier ,const checksum256& trxId, const uint128_t& blockNum, const bytes& sig);

            /**
             * @brief Finalizes registration process.
             * 
             * @param owner Account name of the node owner. 
             * @param status Status of deposit state: 2-> CONFIRMED / 3-> REJECTED
             */
            [[sysio::action]]
            void finalizereg(const name& owner,const uint8_t& status);

            [[sysio::action]]
            void forcereg(const name& owner, const uint8_t& tier);

            /**
             * @brief Creates a new user account on the network and records the sponsor mapping.
             *
             * @ricardian_contract
             * ### Intent
             * The `newuser` action allows a registered tier-1 node owner to create a new user account on the network.
             *
             * ### Obligations
             * - The `creator` must be a registered tier-1 node owner.
             * - The action will create a new account utilizing a new randomly generated username with the provided `pubkey` as its authority.
             * - The registration count for the `creator` will be incremented.
             * - The sponsor mapping (creator, nonce → username) will be recorded in the sponsors table.
             * - The action will fail if the `creator` is not a tier-1 node owner or if the account already exists.
             *
             * ### Rights Granted
             * - The new user account is granted access to the network with the specified public key.
             *
             * @param creator The account name of the tier-1 node owner creating the user
             * @param nonce A unique nonce to ensure the new account name is unique. This can be a random number or a timestamp.
             * @param pubkey The public key to be associated with the new user account.
             * @return The newly created user account name.
             */
            [[sysio::action]]
            name newuser(const name& creator, const name& nonce, const public_key& pubkey);

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
                name owner;             // Account name this policy applies to
                asset net_weight;       // Total NET allocated
                asset cpu_weight;       // Total CPU allocated
                uint64_t ram_bytes;     // Total RAM allocated

                uint64_t primary_key() const { return owner.value; }
            };

            typedef sysio::multi_index<"reslimit"_n, reslimit> reslimit_t;

            /**
             * This table is scoped to Node Owner's acoount names and is used to track all the node registration actions.
             */
            struct [[sysio::table]] nodeownerreg {
                name owner;                     // Node Owners account name 
                uint8_t status;                 // Node Owners registration status 0-> INTENT / 1-> PENDING  / 2-> CONFIRMED / 3-> REJECTED
                checksum256 trx_id;             // Transaction Id of Ethereum deposit
                bytes trx_signature;            // Transaction Signature of Ethereum deposit
                uint8_t tier;                   // Tier of Node Owner
                uint128_t block_num;            // Ethereum Block number the deposit transaction is included in 

                uint64_t primary_key() const { return owner.value; }
                uint64_t by_tier() const { return static_cast<uint64_t>(tier); }
                uint64_t by_status() const {return static_cast<uint64_t>(status); }
                checksum256 by_trxid() const {return trx_id; }
            };

            typedef multi_index<"nodeownerreg"_n, nodeownerreg, 
                indexed_by<"bytier"_n, const_mem_fun<nodeownerreg, uint64_t, &nodeownerreg::by_tier>>,
                indexed_by<"bystatus"_n, const_mem_fun<nodeownerreg, uint64_t, &nodeownerreg::by_status>>,
                indexed_by<"bytrxid"_n, const_mem_fun<nodeownerreg, checksum256, &nodeownerreg::by_trxid>>
            >nodeownerreg_t;


            /**
             * @brief Table mapping (scoped by creator) of nonce to the created username.
             */
            struct [[sysio::table]] sponsor {
                name nonce;
                name username;

                uint64_t primary_key() const { return nonce.value; }
                uint64_t by_username() const { return username.value; }
            };

            typedef sysio::multi_index<
                "sponsors"_n, sponsor,
                indexed_by<"byusername"_n, const_mem_fun<sponsor, uint64_t, &sponsor::by_username>>
            > sponsors_t;

            /**
             * @brief Table tracking how many new users a node owner has sponsored.
             */
            struct [[sysio::table]] sponsorcount {
                name owner;
                uint64_t count;

                uint64_t primary_key() const { return owner.value; }
            };

            typedef sysio::multi_index<"sponsorcount"_n, sponsorcount> sponsorcount_t;

            // ---- Private Functions ----

            /**
             * @brief Registers 'owner' as a Node Owner scoped by network_gen, granting SYS allotment based on Tier and creates a default policy for owner.
             * 
             * NOTE: Can only register for the current generation of the network.
             * 
             * @param owner The account name of the Node Owner.
             * @param tier  The tier of Node they are an owner of. 1, 2, 3
             */
            
            void regnodeowner(const name& owner, const uint8_t& tier);


            /**
             * @brief A simple getter for totall allotted SYS based on tier number: 1, 2, 3. Matches rounding and logic used in activation.
             * 
             * @return An asset containing the amount of SYS this tier gets
             */
            asset get_allocation_for_tier(uint8_t tier);

            struct resources_t {
                asset net;
                asset cpu;
                uint64_t ram_bytes;
            };

            /**
             * @brief Create and set the initial values of reslimit entry
             */
            void set_reslimit(const name& owner, const asset& netWeight, const asset& cpuWeight, int64_t ram_bytes);

            /**
             * @brief Increase values of reslimit entry, assert if not found
             */
            resources_t increase_reslimit(const name& owner, const asset& netWeight, const asset& cpuWeight, int64_t ram_bytes);

    }; // namespace roa
} // namespace sysio
