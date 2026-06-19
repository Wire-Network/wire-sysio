#pragma once

#include <sysio/sysio.hpp>
#include <sysio/kv_table.hpp>
#include <sysio/kv_scoped_table.hpp>
#include <sysio/kv_global.hpp>
#include <sysio/system.hpp> // For current_block_number
#include <sysio/asset.hpp>
#include <sysio/dispatcher.hpp> // For SYSIO_DISPATCH of native action
#include <sysio/privileged.hpp>

namespace sysio {
    class [[sysio::contract("sysio.roa")]] roa : public contract {
        public:
            using contract::contract;

            typedef std::vector<char> bytes;

            /**
             * @brief Initializes sysio.roa, should be called as last step in Bios Boot Sequence, activating the ROA resource management system.
             *
             * @param total_sys The total starting SYS of the network.
             * @param bytes_per_unit The amount of bytes .0001 SYS is worth, set in roastate table. If SYS precision is different, same concept applies, the single smallest unit of the core token.
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
             * @param network_gen Generation of issuer, in cases where you are a Node Owner in multiple, specifies which allocation of SYS to pull from.
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
             * @param network_gen Generation of issuer, in cases where you are a Node Owner in multiple, specifies which allocation of SYS to pull from.
             */
            [[sysio::action]]
            void expandpolicy(const name& owner, const name& issuer, const asset& net_weight, const asset& cpu_weight, const asset& ram_weight, const uint8_t& network_gen);

            /**
             * @brief Increases the policy's time_block extending the policies term.
             *
             * @param owner The account this policy is issued to.
             * @param issuer The Node Owner who issued this policy.
             * @param new_time_block The new block number this policy can't be reduced or reclaimed till.
             */
            [[sysio::action]]
            void extendpolicy(const name& owner, const name& issuer, const uint32_t& new_time_block);

            /**
             * @brief Decrease the resource limits on an existing policy. Subtracts new weights from existing values. Only callable after policy's time_block.
             *
             * Note: Will reclaim UPTO ram_weight worth of bytes, limited to the pool of unused bytes on 'owner's reslimit and upper bound by the policy ram_weight.
             *
             * @param owner The account this policy is issued to.
             * @param issuer The Node Owner who issued this policy.
             * @param net_weight The amount in SYS to decrease NET by.
             * @param cpu_weight The amount in SYS to decrease CPU by.
             * @param ram_weight The amount in SYS to attempt decreasing RAM by, returning only
             * @param network_gen Generation of issuer, in cases where you are a Node Owner in multiple, specifies which allocation of SYS to adjust.
             */
            [[sysio::action]]
            void reducepolicy(const name& owner, const name& issuer, const asset& net_weight, const asset& cpu_weight, const asset& ram_weight, const uint8_t& network_gen);


            /**
             * @brief Directly register `owner` as a node owner at `tier`, bypassing the OPP claim
             *        flow. Privileged bootstrap/test path -- NOT the production registration route.
             *
             * Requires `sysio.roa` active authority, so it is governance/system-only and never
             * user-callable. It exists to seed node owners where the full OPP machinery is absent or
             * unnecessary: the C++ unit-test base tester (registers `nodedaddy` as the default policy
             * issuer for the suite), emissions tests, the TestHarness bios bootstrap, and dev clusters.
             *
             * Production node-owner registration goes through `nodeownreg` instead -- dispatched by
             * the OPP depot (sysio.msgch) on an inbound NodeOwnerRegistration attestation, or
             * governance-signed during bootstrap. Beyond the tier-range check, `forcereg` performs
             * none of `nodeownreg`'s claim-payload validation (no tier name-rule check, no active-key
             * match, no sysio.authex link) and writes no `nodeownerreg` audit row -- it simply
             * allocates the tier's SYS via the shared `regnodeowner` helper.
             *
             * @param owner The account to register as a node owner.
             * @param tier  Node owner tier: 1, 2, or 3.
             */
            [[sysio::action]]
            void forcereg(const name& owner, const uint8_t& tier);

            /**
             * @brief Registers a node owner when the depot (sysio.msgch) processes an inbound OPP
             *        NodeOwnerRegistration attestation. The register step of the NFT claim flow; the
             *        depot inline-sends newnameduser (account create) immediately before this so the
             *        account already exists when nodeownreg runs.
             *
             * Trust-OPP: the OPP envelope is the deposit proof, so this RECORDS the depositor's ETH
             * key as a sysio.authex link (inline recordlink) rather than verifying a pre-existing
             * createlink. Claim-payload problems (bad name, account controlled by a different key,
             * already registered) are soft-failed -- recorded in `nodeownerreg` with a reject_reason
             * and returned -- never thrown, so the dispatching transaction commits. Depot/system
             * invariants (tier range, ETH key type, ROA active) stay hard checks.
             *
             * @param owner        The Wire account to register as node owner (created in-flow if absent).
             * @param tier         Node owner tier: 1, 2, or 3.
             * @param eth_pub_key  The depositor's ETH public key (Wire PUB_EM format); recorded as the link.
             * @param wire_pub_key The Wire account owner/active key the claim specified; an existing
             *                     account must be controlled by exactly this key or the claim is rejected.
             */
            [[sysio::action]]
            void nodeownreg(const name& owner, const uint8_t& tier, const public_key& eth_pub_key,
                            const public_key& wire_pub_key);

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
             * - The sponsor mapping (creator, nonce -> username) will be recorded in the sponsors table.
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

            /**
             * @brief Create a node-owner account with a user-chosen (vanity) name and the holder's
             * K1 key as owner/active, funded with the fixed newaccount_ram from sysio's pool. The
             * create step of the OPP NFT claim flow (create -> createlink -> nodeownreg).
             *
             * Dispatched by the OPP depot (sysio.msgch) as {sysio.roa, active} via delegation, like
             * nodeownreg. Idempotent: a no-op if the account already exists. Tier-based name rules:
             * tier-1 = 2-6 char prefix; tier 2/3 = up to 12 chars.
             *
             * @param account The user-chosen account name.
             * @param pubkey  The holder's K1 public key (becomes owner and active).
             * @param tier    Node-owner tier 1/2/3 (selects the name-length rule).
             */
            [[sysio::action]]
            void newnameduser(const name& account, const public_key& pubkey, uint8_t tier);

            /**
             * @brief Gifts an account exactly the RAM consumed since `usage_before`.
             *
             * Authorized by `sysio.roa` itself: it gifts `get_ram_usage(account) - usage_before`
             * (drawn from sysio's pool), so a change adds only the RAM it actually used.
             * RAM is checked at transaction end, so usage already reflects the change when this
             * runs. The reconciliation is bidirectional: `delta > 0` gifts from sysio's pool,
             * `delta < 0` reclaims back to it (e.g. re-deploying a smaller contract). Inline-called
             * by setsyscode/setsysabi (a follow-on PR drives per-contract gifting). createlink no
             * longer calls giftram -- it records the external-chain link only and bills the row to
             * sysio -- so `sysio.authex` is not an authorizer.
             *
             * @param account      The account to reconcile.
             * @param usage_before The account's `get_ram_usage` snapshot before the change.
             */
            [[sysio::action]]
            void giftram(const name& account, int64_t usage_before);

            /**
             * @brief Deploy a system contract's code to `account`, making it privileged, and gift
             * the exact RAM the code consumes out of sysio's pool (via giftram, measured after).
             * Re-callable: a smaller re-deploy reclaims the freed RAM. Callable by `sysio`.
             *
             * @param account   The account to set code on.
             * @param vmtype    VM type (0 for wasm).
             * @param vmversion VM version (0).
             * @param code      The contract wasm bytes.
             */
            [[sysio::action]]
            void setsyscode(const name& account, uint8_t vmtype, uint8_t vmversion, const bytes& code);

            /**
             * @brief Set a system contract's abi on `account` and gift the exact RAM it consumes
             * out of sysio's pool (via giftram, measured after). Re-callable: a smaller/cleared abi
             * reclaims the freed RAM. Callable by `sysio`.
             *
             * @param account The account to set the abi on.
             * @param abi     The serialized abi bytes.
             */
            [[sysio::action]]
            void setsysabi(const name& account, const bytes& abi);

        public:

            // The `roastate` and `nodeowners` tables below are public so sibling system contracts
            // can read them cross-contract. sysio.chalg resolves the active `network_gen` from
            // `roastate` and point-looks-up a voter's Tier in `nodeowners` for the OPP dispute
            // vote. These tables remain writable only by sysio.roa's own actions.

            /**
             * ROA global-state singleton: activation flag, total SYS, bytes-per-unit, and the
             * active network generation. Public so sibling system contracts can read `network_gen`
             * cross-contract (see the access note above); writable only by sysio.roa's own actions.
             */
            struct [[sysio::table("roastate")]] roa_state {
                bool is_active = false;
                asset total_sys = asset(0, symbol("SYS", 4));
                uint64_t bytes_per_unit = 0;
                uint8_t network_gen = 0; // Network Generation

                SYSLIB_SERIALIZE(roa_state, (is_active)(total_sys)(bytes_per_unit)(network_gen))
            };

            using roastate_t = kv::global<"roastate"_n, roa_state>;

            /**
             * Scoped to network_gen
             *
             * Basic table tracking who T1-3 Node Owners are and their available vs allocated SYS.
             */
            struct nodeowner_key {
                uint64_t owner;
                SYSLIB_SERIALIZE(nodeowner_key, (owner))
            };

            struct [[sysio::table("nodeowners")]] nodeowners {
                name owner;          // Node Owners account name.
                uint8_t tier;        // Represents what tier they hold: 1, 2, or 3
                asset total_sys;     // Total SYS alloted based on tier.
                asset allocated_sys; // Total SYS allocated via policies they issued.
                asset allocated_bw;  // Total SYS allocated to CPU / NET.
                asset allocated_ram; // Total SYS allocated to RAM.
                uint8_t network_gen; // The generation this Node Owner was registered for.

                uint64_t by_tier() const { return static_cast<uint64_t>(tier); }

                SYSLIB_SERIALIZE(nodeowners, (owner)(tier)(total_sys)(allocated_sys)(allocated_bw)(allocated_ram)(network_gen))
            };

            using nodeowners_t = kv::scoped_table<"nodeowners"_n, nodeowner_key, nodeowners,
                kv::index<"bytier"_n, const_mem_fun<nodeowners, uint64_t, &nodeowners::by_tier>>
            >;

        private:

            /**
             * This table is scoped to Node Owner's account names and is used to track all policies issued by Node Owners.
             */
            struct policy_key {
                uint64_t owner;
                SYSLIB_SERIALIZE(policy_key, (owner))
            };

            struct [[sysio::table("policies")]] policies {
                name owner;                 // Account name this policy applies to.
                name issuer;                // Account name of the Node Owner who issued this policy.
                asset net_weight;           // The amount of SYS allocated for NET.
                asset cpu_weight;           // The amount of SYS allocated for CPU.
                asset ram_weight;           // The amount of SYS allocated for RAM.
                uint64_t bytes_per_unit;    // The amount of bytes .0001 SYS was worth when the policy was created.
                uint32_t time_block;        // Block number, this policy can't be deleted or have its values lowered before the networks current block num >= time_block.

                SYSLIB_SERIALIZE(policies, (owner)(issuer)(net_weight)(cpu_weight)(ram_weight)(bytes_per_unit)(time_block))
            };

            using policies_t = kv::scoped_table<"policies"_n, policy_key, policies>;

            /**
             * Scoped to Owner: Holds upper limits of resources an account has access to. This table is used by the Node Operators to maintain usage metrics, replaces 'userres' on sysio.
             */
            struct reslimit_key {
                uint64_t owner;
                SYSLIB_SERIALIZE(reslimit_key, (owner))
            };

            struct [[sysio::table("reslimit")]] reslimit {
                name owner;             // Account name this policy applies to
                asset net_weight;       // Total NET allocated
                asset cpu_weight;       // Total CPU allocated
                uint64_t ram_bytes;     // Total RAM allocated

                SYSLIB_SERIALIZE(reslimit, (owner)(net_weight)(cpu_weight)(ram_bytes))
            };

            using reslimit_t = kv::table<"reslimit"_n, reslimit_key, reslimit>;

            /**
             * This table is scoped to Node Owner's account names and is used to track all the node registration actions.
             */
            struct nodeownerreg_key {
                uint64_t owner;
                SYSLIB_SERIALIZE(nodeownerreg_key, (owner))
            };

            // Registration outcome recorded in `nodeownerreg::status`. Create-in-flow nodeownreg
            // resolves a claim in a single action, recording a terminal status directly -- there is
            // no intermediate INTENT/PENDING state (those belonged to the retired multi-step flow).
            enum reg_status : uint8_t { CONFIRMED = 0, REJECTED = 1 };

            // Rejection reason for `nodeownerreg::reason` (only meaningful when status == REJECTED).
            // Under trust-OPP, nodeownreg soft-fails claim-payload errors by recording one of these
            // instead of aborting the dispatching transaction, so the failure is queryable on Wire.
            // (Values renumbered for the create-in-flow model: the ETH link is now *recorded* via
            // sysio.authex::recordlink rather than verified against a pre-existing createlink, so the
            // former NO_AUTHEX_LINK / KEY_MISMATCH reasons no longer apply. Pre-launch: no stored rows
            // to migrate.)
            enum reject_reason : uint8_t {
                NONE                 = 0,  // not rejected
                NAME_INVALID         = 1,  // chosen account name violates the tier's length rule
                OWNER_NOT_ACCOUNT    = 2,  // account does not exist (creation did not occur)
                ACCOUNT_KEY_MISMATCH = 3,  // existing account's active authority != the single claimed wire key
                DUPLICATE            = 4,  // owner is already a registered node owner
                LINK_KEY_MISMATCH    = 5   // account already carries a different external-chain link key
            };

            // Under trust-OPP the OPP envelope is the deposit proof, so this audit row records only
            // the claim outcome -- no Ethereum tx id / signature / block number is stored (those
            // vestigial fields and the by_trxid index they fed were dropped pre-launch).
            struct [[sysio::table("nodeownerreg")]] nodeownerreg {
                name owner;                     // Node Owners account name
                uint8_t status;                 // Registration outcome (reg_status): 0 -> CONFIRMED / 1 -> REJECTED
                uint8_t tier;                   // Tier of Node Owner
                uint8_t reason;                 // Rejection reason (see reject_reason); NONE(0) unless status == REJECTED

                uint64_t by_tier() const { return static_cast<uint64_t>(tier); }
                uint64_t by_status() const {return static_cast<uint64_t>(status); }

                SYSLIB_SERIALIZE(nodeownerreg, (owner)(status)(tier)(reason))
            };

            using nodeownerreg_t = kv::scoped_table<"nodeownerreg"_n, nodeownerreg_key, nodeownerreg,
                kv::index<"bytier"_n, const_mem_fun<nodeownerreg, uint64_t, &nodeownerreg::by_tier>>,
                kv::index<"bystatus"_n, const_mem_fun<nodeownerreg, uint64_t, &nodeownerreg::by_status>>
            >;


            /**
             * @brief Table mapping (scoped by creator) of nonce to the created username.
             */
            struct sponsor_key {
                uint64_t nonce;
                SYSLIB_SERIALIZE(sponsor_key, (nonce))
            };

            struct [[sysio::table("sponsors")]] sponsor {
                name nonce;
                name username;

                uint64_t by_username() const { return username.value; }

                SYSLIB_SERIALIZE(sponsor, (nonce)(username))
            };

            using sponsors_t = kv::scoped_table<
                "sponsors"_n, sponsor_key, sponsor,
                kv::index<"byusername"_n, const_mem_fun<sponsor, uint64_t, &sponsor::by_username>>
            >;

            /**
             * @brief Table tracking how many new users a node owner has sponsored.
             */
            struct sponsorcount_key {
                uint64_t owner;
                SYSLIB_SERIALIZE(sponsorcount_key, (owner))
            };

            struct [[sysio::table("sponsorcount")]] sponsorcount {
                name owner;
                uint64_t count;

                SYSLIB_SERIALIZE(sponsorcount, (owner)(count))
            };

            using sponsorcount_t = kv::scoped_table<"sponsorcount"_n, sponsorcount_key, sponsorcount>;

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
             * @brief Upsert a row in the `nodeownerreg` audit table. Used by `nodeownreg` to
             *        record the outcome of an OPP claim: CONFIRMED on success, or REJECTED with a
             *        `reject_reason` for a claim-payload failure (trust-OPP soft-fail). Reusing the
             *        existing table keeps the claim outcome queryable on Wire without aborting the
             *        dispatching transaction.
             *
             * @param owner       The claimed account name.
             * @param tier        The claimed tier.
             * @param status      reg_status to record (CONFIRMED or REJECTED).
             * @param reason      reject_reason; reject_reason::NONE for a CONFIRMED row.
             * @param network_gen Generation to scope the audit row under (the caller's live roastate
             *                     network_gen; passed in so this helper does not re-read the singleton).
             */
            void record_nodereg(const name& owner, const uint8_t& tier, uint8_t status, uint8_t reason,
                                uint8_t network_gen);

            /**
             * @brief Whether `account`'s name satisfies the node-owner name-length rule for `tier`.
             *        Tier-1 owners take a short 2-6 char prefix (sub-accounts become <prefix>.<random>);
             *        tier 2/3 take a 1-12 char vanity name. Shared by newnameduser (gates creation) and
             *        nodeownreg (records NAME_INVALID) so the rule lives in one place.
             *
             * @param account The chosen account name.
             * @param tier    Node-owner tier (must already be validated to 1-3).
             * @return true if the name length is valid for the tier.
             */
            static bool valid_name_for_tier(const name& account, uint8_t tier);

            /**
             * @brief Whether `key` can satisfy `account`'s `active` authority by itself -- i.e. it
             *        appears among the active keys with weight >= threshold. Passes for a
             *        newnameduser-created account ({key}) and for a standard account that also carries
             *        the benign <account>@sysio.code entry; rejects an account a different key controls
             *        (that key won't be present) and a multi-sig where `key` alone is insufficient.
             *
             * @param account The account whose active permission to inspect.
             * @param key     The claimed owner/active public key.
             * @return true if `key` alone controls `account`'s active permission.
             */
            static bool active_key_matches(const name& account, const public_key& key);


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
            void set_reslimit(const name& owner, const asset& net_weight, const asset& cpu_weight, int64_t ram_bytes);

            /**
             * @brief Increase values of reslimit entry, assert if not found
             */
            resources_t increase_reslimit(const name& owner, const asset& net_weight, const asset& cpu_weight, int64_t ram_bytes,
                                          bool require_to_exist);

            /**
             * @brief Decrease ram_bytes on an existing reslimit entry
             */
            void decrease_reslimit(const name& owner, uint64_t ram_bytes);

    }; // namespace roa
} // namespace sysio
