#include "sysio.roa.hpp"
#include "sysio.system/native.hpp"
#include "sysio.system/emissions.hpp"

#include <sysio.authex/sysio.authex.hpp>
#include <sysio/permission.hpp>   // get_permission -- read an account's active authority in nodeownreg

namespace sysio {

    namespace {
        // sysio.authex identifiers used by the node-owner depot dispatch (invariant: no magic
        // literals -- a contract rename is one change here, not scattered across call sites).
        constexpr name AUTHEX_ACCOUNT    = "sysio.authex"_n;
        constexpr name AUTHEX_RECORDLINK = "recordlink"_n;
    } // anonymous namespace

    bool is_sysio_account(const name& account) {
        return account.prefix() == "sysio"_n;
    }

    void roa::set_reslimit(const name& owner, const asset& net_weight, const asset& cpu_weight, int64_t ram_bytes) {
        bool sysio_acct = is_sysio_account(owner);

        reslimit_t reslimit(get_self());
        auto key = reslimit_key{owner.value};
        check(!reslimit.contains(key), "Resource limit already exist for this owner");

        resources_t res = {
            .net = sysio_acct ? asset(0, net_weight.symbol) : net_weight,
            .cpu = sysio_acct ? asset(0, cpu_weight.symbol) : cpu_weight,
            .ram_bytes = (uint64_t)ram_bytes
        };
        reslimit.emplace(get_self(), key, roa::reslimit{
            .owner = owner,
            .net_weight = res.net,
            .cpu_weight = res.cpu,
            .ram_bytes = res.ram_bytes,
        });
    }

    roa::resources_t roa::increase_reslimit(const name& owner, const asset& net_weight, const asset& cpu_weight, int64_t ram_bytes, bool require_to_exist) {
        bool sysio_acct = is_sysio_account(owner);

        reslimit_t reslimit(get_self());
        auto key = reslimit_key{owner.value};
        bool exists = reslimit.contains(key);
        check(!require_to_exist || exists, "No resource limit exists for this account.");

        if (!exists) {
            // add newaccount_ram for reslimit when created to account for gifted ram when created
            resources_t res = {
                .net = sysio_acct ? asset(0, net_weight.symbol) : net_weight,
                .cpu = sysio_acct ? asset(0, cpu_weight.symbol) : cpu_weight,
                .ram_bytes = (uint64_t)ram_bytes + sysiosystem::newaccount_ram
            };
            reslimit.emplace(get_self(), key, roa::reslimit{
                .owner = owner,
                .net_weight = res.net,
                .cpu_weight = res.cpu,
                .ram_bytes = res.ram_bytes,
            });
            return res;
        } else {
            reslimit.modify(get_self(), key, [&](auto& row) {
                if (!sysio_acct) {
                    row.net_weight.amount += net_weight.amount;
                    row.cpu_weight.amount += cpu_weight.amount;
                }
                check(ram_bytes >= 0, "increase_reslimit does not allow negative ram_bytes");
                row.ram_bytes += static_cast<uint64_t>(ram_bytes);
            });
            auto updated = reslimit.get(key);
            return resources_t{
                .net = updated.net_weight,
                .cpu = updated.cpu_weight,
                .ram_bytes = updated.ram_bytes
            };
        }
    }

    void roa::decrease_reslimit(const name& owner, uint64_t ram_bytes) {
        reslimit_t reslimit(get_self());
        auto key = reslimit_key{owner.value};
        auto row = reslimit.get(key, "No resource limit exists for this account.");
        check(row.ram_bytes >= ram_bytes, "reslimit ram underflow");
        reslimit.modify(get_self(), key, [&](auto& row) {
            row.ram_bytes -= ram_bytes;
        });
    }

    void roa::activateroa(const asset& total_sys, const uint64_t& bytes_per_unit) {
        require_auth(get_self());

        roastate_t roastate(get_self());
        auto state = roastate.get_or_default(roa_state{});

        check(!state.is_active, "Contract already activated.");
        check(total_sys.symbol == symbol("SYS", 4), "Total SYS must be SYS.");

        state.is_active = true;
        state.total_sys = total_sys;
        state.bytes_per_unit = bytes_per_unit;
        state.network_gen = 0;
        roastate.set(state, get_self());

        const int64_t total_amount = total_sys.amount; // smallest units

        // Fractions per node (rational approach). Tier counts come from the
        // shared constants in sysio.system/emissions.hpp so this matches
        // sysio.system::addnodeowner's per-tier cap exactly.
        // T1: 4% = 4/100 (add 50 for rounding)
        int64_t t1_per_node = (total_amount * 4 + 50) / 100;
        int64_t t1_total = t1_per_node * sysiosystem::emissions::T1_MAX_NODE_OWNERS;

        // T2: 0.0015 = 15/10,000 (add 5,000 for rounding)
        int64_t t2_per_node = (total_amount * 15 + 5000) / 10000;
        int64_t t2_total = t2_per_node * sysiosystem::emissions::T2_MAX_NODE_OWNERS;

        // T3: 0.00003 = 3/100,000 (add 50,000 for rounding)
        int64_t t3_per_node = (total_amount * 3 + 50000) / 100000;
        int64_t t3_total = t3_per_node * sysiosystem::emissions::T3_MAX_NODE_OWNERS;

        // Allocated sum
        int64_t allocated = t1_total + t2_total + t3_total;

        // Leftover
        int64_t leftover = total_amount - allocated;

        // Convert the leftover (SYS units) to bytes and partition it so the grand total of all
        // reslimits stays exactly total_sys * bytes_per_unit — nothing is minted on top:
        //   T = node-owner reserve (allocated above) + roa allocation + sysio pool.
        // sysio.roa keeps half the leftover for its own (growing) bookkeeping tables; sysio gets
        // the rest as THE pool that funds account creation and every other system contract's RAM
        // (deployed via setsyscode/setsysabi, which gift the exact bytes out of this pool). Other
        // system contracts are deliberately NOT pre-allocated here — they self-fund exactly. The
        // only deduction is the sysio.acct account-creation bucket seed, taken out of sysio's
        // share so it stays conserved.
        uint64_t leftover_bytes = (uint64_t)(leftover * bytes_per_unit);
        uint64_t roa_ram_bytes = leftover_bytes / 2;
        const uint64_t acct_seed_bytes = sysiosystem::newaccount_ram;
        uint64_t sysio_gross = leftover_bytes - roa_ram_bytes;
        check(sysio_gross > acct_seed_bytes, "Leftover RAM too small for the account-creation seed");
        uint64_t sysio_ram_bytes = sysio_gross - acct_seed_bytes;

        // Create/set reslimit for sysio.roa (self)
        set_reslimit(get_self(), asset(0, total_sys.symbol), asset(0, total_sys.symbol), roa_ram_bytes);

        // Set sysio.roas new account limits.
        set_resource_limits(get_self(), roa_ram_bytes, -1, -1);

        // Create/set reslimit for sysio (the funding pool)
        name sys_account = "sysio"_n;
        set_reslimit(sys_account, asset(0, total_sys.symbol), asset(0, total_sys.symbol), sysio_ram_bytes);

        // Set sysio new account limits.
        set_resource_limits(sys_account, sysio_ram_bytes, -1, -1);

        // Add policy for sys_account for tracking RAM provided to accounts for account creation
        policies_t policies(get_self(), sys_account.value);
        policies.emplace(get_self(), policy_key{"sysio.acct"_n.value}, roa::policies{
            .owner = "sysio.acct"_n,
            .issuer = sys_account,
            .net_weight = asset(0, total_sys.symbol),
            .cpu_weight = asset(0, total_sys.symbol),
            .ram_weight = asset(0, total_sys.symbol),
            .bytes_per_unit = state.bytes_per_unit,
            .time_block = 0,
        });
        // Seed the sysio.acct account-creation bucket, funded out of sysio's bucket above
        // (deducted from sysio_gross). No CPU/NET.
        set_reslimit("sysio.acct"_n, asset(0, total_sys.symbol), asset(0, total_sys.symbol), acct_seed_bytes);
        set_resource_limits("sysio.acct"_n, acct_seed_bytes, 0, 0);


    };

    void roa::setbyteprice(const uint64_t& bytes_per_unit) {
        require_auth(get_self());

        roastate_t roastate(get_self());

        // Gets values in the table.
        auto state = roastate.get();

        // Make sure ROA 'is_active' first.
        check(state.is_active, "ROA is not currently active");
        check(sysiosystem::newaccount_ram == (sysiosystem::newaccount_ram / bytes_per_unit) * bytes_per_unit,
              "newaccount_ram needs to be evenly divisable to avoid dust");

        state.bytes_per_unit = bytes_per_unit;

        // Set values to table.
        roastate.set(state, get_self());
    };

    void add_system_resources(const name& owner, int64_t net, int64_t cpu, int64_t ram) {
        bool sysio_acct = is_sysio_account(owner);
        // Get current resource limits
        int64_t current_ram, current_net, current_cpu;
        get_resource_limits(owner, current_ram, current_net, current_cpu);
        if (current_ram < 0) current_ram = 0; // if unlimited use the new limit

        // Set the system resource limits
        auto updated_ram = current_ram + ram;
        if (sysio_acct) {
            // Infinite CPU/NET for sysio accounts
            set_resource_limits(owner, updated_ram, -1, -1);
        } else {
            if (current_net < 0) current_net = 0; // if unlimited use the new limit
            if (current_cpu < 0) current_cpu = 0; // if unlimited use the new limit
            auto updated_net = current_net + net;
            auto updated_cpu = current_cpu + cpu;
            set_resource_limits(owner, updated_ram, updated_net, updated_cpu);
        }
    }

    void roa::giftram(const name& account, int64_t usage_before) {
        // Authorized by sysio.roa itself (setsyscode/setsysabi inline-call it; a follow-on PR
        // drives it for per-contract RAM gifting). createlink no longer calls giftram -- it
        // records the EM link only and bills the row to sysio -- so sysio.authex is not an authorizer.
        require_auth(get_self());
        check(is_account(account), "account does not exist");

        // Unlimited-RAM accounts (privileged/system, or not yet ROA-managed) have no RAM
        // constraint to satisfy and must NOT be shrunk to a finite limit — skip the gift.
        // (The delta>0 path is exercised when a finite account's RAM grows -- e.g. setsyscode
        //  deploying a contract, or the depot-driven node-owner flow; see the nodeownreg tests' TODO.)
        int64_t cur_ram, cur_net, cur_cpu;
        get_resource_limits(account, cur_ram, cur_net, cur_cpu);
        if (cur_ram < 0) return;

        // Reconcile `account`'s gifted RAM to its *exact* current usage. The preceding inline
        // action (setcode / setabi) already ran, so usage reflects the change. RAM is
        // checked at transaction end, so the transient over/under in between is fine.
        //   delta > 0 → gift from sysio's pool;  delta < 0 → reclaim back to it (e.g. re-deploying
        //   a smaller contract returns RAM). Always a *conserving transfer* with sysio — never a
        //   mint — so total RAM stays accounted for on both the reslimit books and chain quota,
        //   exactly like account creation (newuser/transfer_ram).
        int64_t delta = get_ram_usage(account) - usage_before;
        if (delta == 0) return;

        roastate_t roastate(get_self());
        auto state = roastate.get();
        check(state.is_active, "ROA is not active yet");
        auto sym = state.total_sys.symbol;

        if (delta > 0) {
            // reslimit: sysio pool -> sysio.acct bucket. sysio.acct's row exists from activateroa.
            increase_reslimit("sysio.acct"_n, asset(0, sym), asset(0, sym), delta, true);
            decrease_reslimit("sysio"_n, delta);
            // chain quota: move delta from sysio to the account (not add_system_resources alone,
            // which would mint). The check bounds the gift by sysio's remaining pool.
            int64_t sram, snet, scpu;
            get_resource_limits("sysio"_n, sram, snet, scpu);
            check(sram >= delta, "sysio RAM pool exhausted");
            set_resource_limits("sysio"_n, sram - delta, snet, scpu);
            add_system_resources(account, 0, 0, delta);
        } else {
            // Reclaim `r` back to sysio's pool — the exact reverse of the gift path.
            uint64_t r = static_cast<uint64_t>(-delta);
            decrease_reslimit("sysio.acct"_n, r);
            increase_reslimit("sysio"_n, asset(0, sym), asset(0, sym), static_cast<int64_t>(r), true);
            int64_t aram, anet, acpu;
            get_resource_limits(account, aram, anet, acpu);
            check(aram >= static_cast<int64_t>(r), "account RAM underflow on reclaim");
            set_resource_limits(account, aram - static_cast<int64_t>(r), anet, acpu);
            int64_t sram, snet, scpu;
            get_resource_limits("sysio"_n, sram, snet, scpu);
            set_resource_limits("sysio"_n, sram + static_cast<int64_t>(r), snet, scpu);
        }
    }

    void roa::setsyscode(const name& account, uint8_t vmtype, uint8_t vmversion, const bytes& code) {
        require_auth("sysio"_n);
        roastate_t roastate(get_self());
        check(roastate.get().is_active, "ROA is not active yet");
        check(is_account(account), "account does not exist");

        int64_t usage_before = get_ram_usage(account);

        // 1) Deploy the code (bills RAM to `account`).
        action(permission_level{account, "active"_n}, "sysio"_n, "setcode"_n,
               std::make_tuple(account, vmtype, vmversion, code)).send();
        // 2) Make it privileged (system contract). Must run AFTER setcode — the chain rejects
        //    setpriv on a codeless account ("setcode must be called before setpriv"). Inline so it
        //    is ordered after (1); roa is privileged so it can declare sysio's auth. Idempotent.
        action(permission_level{"sysio"_n, "active"_n}, "sysio"_n, "setpriv"_n,
               std::make_tuple(account, static_cast<uint8_t>(1))).send();
        // 3) Reconcile `account`'s gifted RAM to its exact new usage out of sysio's pool — giftram
        //    runs after (1)+(2) (depth-first inline) and reclaims if the new code is smaller.
        action(permission_level{get_self(), "active"_n}, get_self(), "giftram"_n,
               std::make_tuple(account, usage_before)).send();
    }

    void roa::setsysabi(const name& account, const bytes& abi) {
        require_auth("sysio"_n);
        roastate_t roastate(get_self());
        check(roastate.get().is_active, "ROA is not active yet");
        check(is_account(account), "account does not exist");

        // Set the abi (bills RAM to `account`), then reconcile gifted RAM exactly from sysio's
        // pool — reclaims if the new abi is smaller (or cleared).
        int64_t usage_before = get_ram_usage(account);
        action(permission_level{account, "active"_n}, "sysio"_n, "setabi"_n,
               std::make_tuple(account, abi)).send();
        action(permission_level{get_self(), "active"_n}, get_self(), "giftram"_n,
               std::make_tuple(account, usage_before)).send();
    }

    void roa::addpolicy(const name& owner, const name& issuer, const asset& net_weight, const asset& cpu_weight, const asset& ram_weight,
                        const uint32_t& time_block, const uint8_t& network_gen)
    {
        // Can only issue policies if you are the issuer.
        require_auth(issuer);

        // Ensure ROA is active
        roastate_t roastate(get_self());
        auto state = roastate.get();
        check(state.is_active, "ROA is not currently active");
        check(network_gen <= state.network_gen, "Invalid network generation.");

        // Ensure issuer is a node owner for the specified network_gen
        nodeowners_t nodeowners(get_self(), network_gen);
        auto node_key = nodeowner_key{issuer.value};
        check(nodeowners.contains(node_key), "Only Node Owners can issue policies for this generation.");
        auto node = nodeowners.get(node_key);

        // Validate weights
        check(net_weight.amount >= 0, "NET weight cannot be negative");
        check(cpu_weight.amount >= 0, "CPU weight cannot be negative");
        check(ram_weight.amount >= 0, "RAM weight cannot be negative");

        // If owner is sysio or sysio.* account, no CPU/NET allowed
        bool sysio_acct = is_sysio_account(owner);
        if (sysio_acct) {
            check(net_weight.amount == 0 && cpu_weight.amount == 0, "Cannot allocate CPU/NET to sysio accounts.");
        }

        // Check that at least one is non-zero
        check(!(net_weight.amount == 0 && cpu_weight.amount == 0 && ram_weight.amount == 0), "At least one of NET, CPU, or RAM must be allocated.");

        // Total new allocation
        asset total_new_allocation = net_weight + cpu_weight + ram_weight;

        // Ensure issuer has enough unallocated SYS
        asset free_sys = node.total_sys - node.allocated_sys;
        check(total_new_allocation.amount <= free_sys.amount, "Not enough unallocated SYS for this policy.");

        // Calculate RAM bytes
        int64_t ram_bytes_to_allocate = (int64_t)ram_weight.amount * (int64_t)state.bytes_per_unit;

        // Check if a policy already exists for this owner from this issuer
        policies_t policies(get_self(), issuer.value);
        auto pol_key = policy_key{owner.value};
        check(!policies.contains(pol_key), "A policy for this owner already exists from this issuer. Use expandpolicy instead.");

        // Create/set reslimit for the 'owner', add newaccount_ram since reslimit is being created for the user
        increase_reslimit(owner, net_weight, cpu_weight, ram_bytes_to_allocate, false);

        // Update the system resource limits
        add_system_resources(owner, net_weight.amount, cpu_weight.amount, ram_bytes_to_allocate);

        // Add the new policy
        policies.emplace(get_self(), pol_key, roa::policies{
            .owner = owner,
            .issuer = issuer,
            .net_weight = net_weight,
            .cpu_weight = cpu_weight,
            .ram_weight = ram_weight,
            .bytes_per_unit = state.bytes_per_unit,
            .time_block = time_block,
        });

        // Update the issuer's allocations
        nodeowners.modify(get_self(), node_key, [&](auto& row) {
            row.allocated_sys.amount += total_new_allocation.amount;
            row.allocated_bw.amount += (net_weight.amount + cpu_weight.amount);
            row.allocated_ram.amount += ram_weight.amount;
        });
    };

    void roa::expandpolicy(const name& owner, const name& issuer, const asset& net_weight, const asset& cpu_weight, const asset& ram_weight,
                           const uint8_t& network_gen)
    {
        require_auth(issuer);

        // Ensure issuer is a node owner in the given generation
        nodeowners_t nodeowners(get_self(), network_gen);
        auto node_key = nodeowner_key{issuer.value};
        check(nodeowners.contains(node_key), "Only Node Owners can manage policies.");
        auto node = nodeowners.get(node_key);

        // Fetch issuer's policies scoped by issuer
        policies_t policies(get_self(), issuer.value);
        auto pol_key = policy_key{owner.value};
        auto pol = policies.get(pol_key, "You have no policy for this owner.");

        // Validate weights (zero increments allowed)
        check(net_weight.amount >= 0, "NET weight cannot be negative");
        check(cpu_weight.amount >= 0, "CPU weight cannot be negative");
        check(ram_weight.amount >= 0, "RAM weight cannot be negative");

        // Check that at least one of them is non-zero
        if (net_weight.amount == 0 && cpu_weight.amount == 0 && ram_weight.amount == 0) {
            check(false, "At least one of NET, CPU, or RAM must be increased.");
        }

        bool sysio_acct = is_sysio_account(owner);
        if (sysio_acct) {
            // Cannot allocate CPU/NET to sysio accounts
            check(net_weight.amount == 0 && cpu_weight.amount == 0, "Cannot allocate CPU/NET to sysio accounts.");
        }

        // Calculate total new allocation
        asset total_new_allocation = net_weight + cpu_weight + ram_weight;

        // Ensure the issuer has enough unallocated SYS
        asset free_sys = node.total_sys - node.allocated_sys;
        check(total_new_allocation.amount <= free_sys.amount, "Issuer does not have enough unallocated SYS for this policy expansion.");

        // Convert RAM weight to bytes
        int64_t ram_bytes_to_allocate = (int64_t)ram_weight.amount * (int64_t)pol.bytes_per_unit;

        // Update the policy weights
        policies.modify(get_self(), pol_key, [&](auto& row) {
            if (!sysio_acct) {
                row.net_weight.amount += net_weight.amount;
                row.cpu_weight.amount += cpu_weight.amount;
            }
            row.ram_weight.amount += ram_weight.amount;
        });

        // Update issuer's allocations in nodeowners
        nodeowners.modify(get_self(), node_key, [&](auto& row) {
            row.allocated_sys.amount += total_new_allocation.amount;
            row.allocated_bw.amount += (net_weight.amount + cpu_weight.amount);
            row.allocated_ram.amount += ram_weight.amount;
        });

        // Update owner's resource limits
        increase_reslimit(owner, net_weight, cpu_weight, ram_bytes_to_allocate, true);
        // Update the system resource limits
        add_system_resources(owner, net_weight.amount, cpu_weight.amount, ram_bytes_to_allocate);
    };

    void roa::extendpolicy(const name& owner, const name& issuer, const uint32_t& new_time_block) {
        require_auth(issuer);

        // Pointer to Policies table
        policies_t policies(get_self(), issuer.value);
        auto pol_key = policy_key{owner.value};
        auto pol = policies.get(pol_key, "Policy does not exist under this issuer for this owner");

        check(new_time_block > pol.time_block, "Cannot reduce a policies existing time_block");
        check(new_time_block > current_block_number(), "You cannot set a time_block lower than the current block");

        policies.modify(get_self(), pol_key, [&](auto& row) {
            row.time_block = new_time_block;
        });
    };

    void roa::reducepolicy(const name& owner, const name& issuer, const asset& net_weight, const asset& cpu_weight, const asset& ram_weight,
                           const uint8_t& network_gen)
    {
        require_auth(issuer);

        // Ensure issuer is a node owner in the given generation
        nodeowners_t nodeowners(get_self(), network_gen);
        auto node_key = nodeowner_key{issuer.value};
        check(nodeowners.contains(node_key), "Only Node Owners can manage policies.");

        // Fetch issuer's policies scoped by issuer
        policies_t policies(get_self(), issuer.value);
        auto pol_key = policy_key{owner.value};
        auto pol_row = policies.get(pol_key, "You have no policy for this owner.");

        // Validate time block
        uint32_t current_block = current_block_number();
        check(current_block >= pol_row.time_block, "Cannot reduce policy before time_block");

        // Ensure we don't reduce below zero
        check(net_weight.amount <= pol_row.net_weight.amount, "Cannot reduce NET below zero");
        check(cpu_weight.amount <= pol_row.cpu_weight.amount, "Cannot reduce CPU below zero");
        check(ram_weight.amount <= pol_row.ram_weight.amount, "Cannot reduce RAM below zero");

        // Special sysio check
        bool sysio_acct = is_sysio_account(owner);

        reslimit_t reslimit(get_self());
        auto res_key = reslimit_key{owner.value};
        auto res = reslimit.get(res_key, "reslimit row does not exist for this owner");

        // Adjust resource limits
        int64_t ram_bytes, net_limit, cpu_limit;
        get_resource_limits(owner, ram_bytes, net_limit, cpu_limit);

        uint64_t bytes_per_unit = pol_row.bytes_per_unit;
        int64_t divisible_ram_to_reclaim = 0;
        asset reclaimed_ram_weight(0, ram_weight.symbol);

        if (ram_weight.amount > 0) {
            int64_t ram_usage = get_ram_usage(owner);
            int64_t ram_unused = ram_bytes - ram_usage;
            int64_t requested_ram_bytes = ram_weight.amount * (int64_t) bytes_per_unit;
            int64_t ram_to_reclaim = std::min(ram_unused, requested_ram_bytes);

            check(ram_to_reclaim >= 0, "Invalid RAM reclaim calculation");

            int64_t remainder = ram_to_reclaim % (int64_t) bytes_per_unit;
            divisible_ram_to_reclaim = ram_to_reclaim - remainder;

            int64_t reclaimed_units = divisible_ram_to_reclaim / (int64_t) bytes_per_unit;
            reclaimed_ram_weight = asset(reclaimed_units, ram_weight.symbol);

            check(reclaimed_ram_weight.amount <= ram_weight.amount, "Cannot reclaim more RAM than requested");
        }

        int64_t new_net = (net_limit < 0) ? -1 : std::max((int64_t)0, net_limit - net_weight.amount);
        int64_t new_cpu = (cpu_limit < 0) ? -1 : std::max((int64_t)0, cpu_limit - cpu_weight.amount);
        set_resource_limits(
            owner,
            ram_bytes - divisible_ram_to_reclaim,
            new_net,
            new_cpu
        );

        // Update reslimit row
        reslimit.modify(get_self(), res_key, [&](auto& row) {
            if (!sysio_acct) {
                row.net_weight.amount -= net_weight.amount;
                row.cpu_weight.amount -= cpu_weight.amount;
            }
            row.ram_bytes -= static_cast<uint64_t>(divisible_ram_to_reclaim);
        });

        // Update / remove the policies row
        pol_row.net_weight -= net_weight;
        pol_row.cpu_weight -= cpu_weight;
        pol_row.ram_weight -= reclaimed_ram_weight;

        bool all_zero = (pol_row.net_weight.amount == 0 &&
                         pol_row.cpu_weight.amount == 0 &&
                         pol_row.ram_weight.amount == 0);
        if (all_zero) {
            policies.erase(pol_key);
        } else {
            policies.modify(get_self(), pol_key, [&](auto& row) {
                if (!sysio_acct) {
                    row.net_weight.amount = pol_row.net_weight.amount;
                    row.cpu_weight.amount = pol_row.cpu_weight.amount;
                }
                row.ram_weight.amount = pol_row.ram_weight.amount;
            });
        }

        // Update nodeowners row
        int64_t total_reclaimed_sys_amount = net_weight.amount + cpu_weight.amount + reclaimed_ram_weight.amount;
        nodeowners.modify(get_self(), node_key, [&](auto &row) {
            row.allocated_sys.amount -= total_reclaimed_sys_amount;
            row.allocated_bw.amount -= (net_weight.amount + cpu_weight.amount);
            row.allocated_ram.amount -= reclaimed_ram_weight.amount;
        });
    };

    void roa::forcereg(const name& owner, const uint8_t& tier) {
        require_auth(get_self());

        check(tier > 0 && tier <= 3, "Tier level must be between 1 and 3");

        regnodeowner(owner, tier);
    };

    void roa::nodeownreg(const name& owner, const uint8_t& tier, const public_key& eth_pub_key,
                         const public_key& wire_pub_key) {
        // Dispatched by the OPP depot (sysio.msgch) when it processes an inbound
        // ATTESTATION_TYPE_NODE_OWNER_REG attestation. msgch inline-sends newnameduser (account
        // create) and then this action, both declaring permission_level{sysio.roa, active}; the
        // chain accepts that declaration because sysio.roa.active trusts msgch@sysio.code via a
        // code-permission delegation wired at bootstrap (same shape as the sysio.opreg grant). So
        // require_auth(get_self()) is the correct gate: only the delegated depot dispatch satisfies
        // it. Inline actions run depth-first, so newnameduser's newaccount has already executed and
        // `owner` exists by the time this runs.
        require_auth(get_self());

        // ---- Envelope / system invariants (depot misuse) ----
        // Trust-OPP: well-formed traffic cannot trip these, so a failure means the depot has a bug
        // or the system is misconfigured -- abort loudly. These are NOT claim-payload errors.
        check(tier > 0 && tier <= 3, "Tier level must be between 1 and 3");
        // NFT deposits land on Ethereum, so the recorded link is always an EM (secp256k1) key.
        check(eth_pub_key.index() == fc::crypto::key_type_em,
              "eth_pub_key must be an EM (secp256k1) public key");

        // ---- Claim-payload checks (trust-OPP soft-fail) ----
        // The OPP envelope can be well-formed while the claim inside it is bad (the name the user
        // chose is invalid, names an account they do not control, or is a replay). Aborting the
        // dispatching transaction would be bad UX, leave no audit row, and give the batch operator
        // undefined retry semantics. Instead record a REJECTED row with a reason and return, so the
        // transaction commits and the failure is queryable on Wire (no outbound error attestation).

        // (1) the chosen name must be valid for the tier (newnameduser skipped creation otherwise).
        if (!valid_name_for_tier(owner, tier)) {
            record_nodereg(owner, tier, REJECTED, NAME_INVALID);
            return;
        }

        // (2) the account must exist. newnameduser creates it in-flow; a valid name that still has
        // no account means creation did not occur (defensive -- normally unreachable).
        if (!is_account(owner)) {
            record_nodereg(owner, tier, REJECTED, OWNER_NOT_ACCOUNT);
            return;
        }

        // (3) the account must be controlled by exactly the claimed wire key. For a just-created
        // account this is the key newnameduser set; for a pre-existing account it proves the claimant
        // controls it -- an NFT cannot be claimed into an account someone else holds.
        if (!active_key_matches(owner, wire_pub_key)) {
            record_nodereg(owner, tier, REJECTED, ACCOUNT_KEY_MISMATCH);
            return;
        }

        // (4) must not already be a registered node owner -- a replay / double-claim. Record
        // DUPLICATE and return so the claim is idempotent and auditable. ROA-active is a hard
        // system invariant (the network cannot function with ROA inactive).
        {
            roastate_t roastate(get_self());
            auto state = roastate.get();
            check(state.is_active, "ROA is not active yet");
            nodeowners_t nodeowners(get_self(), state.network_gen);
            if (nodeowners.contains(nodeowner_key{owner.value})) {
                record_nodereg(owner, tier, REJECTED, DUPLICATE);
                return;
            }
        }

        // Record the depositor's ETH key as a sysio.authex link via the trusted depot-only path.
        // recordlink requires sysio.authex.active, satisfied by the sysio.roa@sysio.code delegation
        // on authex; it is idempotent and non-throwing. EVM-only by design (NFT deposits originate
        // on Ethereum); to extend to another ChainKind, promote the kind to an action parameter.
        action(permission_level{AUTHEX_ACCOUNT, "active"_n}, AUTHEX_ACCOUNT, AUTHEX_RECORDLINK,
               std::make_tuple(owner, opp::types::ChainKind::CHAIN_KIND_EVM, eth_pub_key)).send();

        regnodeowner(owner, tier);
        record_nodereg(owner, tier, CONFIRMED, NONE);
    };

    // ---- Private Helper Function ----

    void roa::record_nodereg(const name& owner, const uint8_t& tier, uint8_t status, uint8_t reason) {
        roastate_t roastate(get_self());
        auto state = roastate.get();

        nodeownerreg_t nodereg(get_self(), state.network_gen);
        auto reg_key = nodeownerreg_key{owner.value};

        auto write = [&](auto& row) {
            row.owner  = owner;
            row.status = status;
            row.tier   = tier;
            row.reason = reason;
            // trx_id / trx_signature / block_num are unused under trust-OPP (the OPP envelope is the
            // deposit proof); leave them at their default-constructed values.
        };

        if (nodereg.contains(reg_key)) {
            nodereg.modify(get_self(), reg_key, write);
        } else {
            nodeownerreg row{};
            write(row);
            nodereg.emplace(get_self(), reg_key, row);
        }
    }

    bool roa::valid_name_for_tier(const name& account, uint8_t tier) {
        const size_t len = account.length();
        // Tier-1 owners take a short 2-6 char prefix (sub-accounts become <prefix>.<random>);
        // tier 2/3 take a 1-12 char vanity name.
        if (tier == 1) return len >= 2 && len <= 6;
        return len >= 1 && len <= 12;
    }

    bool roa::active_key_matches(const name& account, const public_key& key) {
        // The claim is valid iff `key` can satisfy the account's `active` authority by itself: it
        // appears among the keys with weight >= threshold. We deliberately do NOT require a single-key
        // authority or an empty `accounts` list -- a standard account also carries an
        // <account>@sysio.code entry (benign: it grants the account's own contract inline-action
        // rights, not external control), and newnameduser-created accounts have neither. A claim into
        // an account a different key controls still fails: that key won't be among `auth.keys`.
        auto perm = get_permission(account, "active"_n);
        if (!perm.has_value() || perm->auth.threshold == 0) return false;
        for (const auto& kw : perm->auth.keys) {
            if (kw.key == key && kw.weight >= perm->auth.threshold) return true;
        }
        return false;
    }

    void roa::regnodeowner(const name& owner, const uint8_t& tier) {

        roastate_t roastate(get_self());
        auto state = roastate.get();
        check(state.is_active, "ROA is not active yet");

        nodeowners_t nodeowners(get_self(), state.network_gen);
        auto node_key = nodeowner_key{owner.value};
        check(!nodeowners.contains(node_key), "This account is already registered.");

        // Get the total SYS allocation for this tier
        asset total_sys_allocation = get_allocation_for_tier(tier);

        // We will track how much is allocated to bandwidth (CPU/NET) and RAM separately
        asset allocated_sys(0, state.total_sys.symbol);
        asset allocated_bw(0, state.total_sys.symbol);
        asset allocated_ram(0, state.total_sys.symbol);

        // Personal RAM allocation: For example 0.0080 SYS
        asset personal_ram_weight(80, state.total_sys.symbol); // 0.0080 SYS if precision=4
        uint64_t personal_ram_bytes = personal_ram_weight.amount * state.bytes_per_unit;
        allocated_sys += personal_ram_weight;
        allocated_ram += personal_ram_weight; // RAM allocation

        // 10% of total SYS goes to sysio for RAM
        int64_t sysio_alloc_amount = total_sys_allocation.amount / 10;
        asset sysio_allocation(sysio_alloc_amount, total_sys_allocation.symbol);
        allocated_sys += sysio_allocation;
        allocated_ram += sysio_allocation; // Also RAM allocation since it's for sysio policy

        // Minimal default net/cpu: 0.0500 SYS each for NET and CPU (0.1000 SYS total)
        asset net_cpu_weight(500, state.total_sys.symbol); // 0.0500 SYS
        allocated_sys += (net_cpu_weight + net_cpu_weight);
        allocated_bw += (net_cpu_weight + net_cpu_weight); // CPU/NET allocation

        policies_t policies(get_self(), owner.value);
        auto pol_key = policy_key{owner.value};

        name sysio_account = "sysio"_n;
        asset zero_asset(0, state.total_sys.symbol);

        if (!policies.contains(pol_key)) {
            // Create personal policy
            policies.emplace(get_self(), pol_key, roa::policies{
                .owner = owner,
                .issuer = owner,
                .net_weight = net_cpu_weight,
                .cpu_weight = net_cpu_weight,
                .ram_weight = personal_ram_weight,
                .bytes_per_unit = state.bytes_per_unit,
                .time_block = 1,
            });

            // Create sysio policy for RAM
            policies.emplace(get_self(), policy_key{sysio_account.value}, roa::policies{
                .owner = sysio_account,
                .issuer = owner,
                .net_weight = zero_asset,
                .cpu_weight = zero_asset,
                .ram_weight = sysio_allocation,
                .bytes_per_unit = state.bytes_per_unit,
                .time_block = UINT32_MAX, // do not allow to be extended
            });
        }

        // Owner reslimits
        set_reslimit(owner, net_cpu_weight, net_cpu_weight, personal_ram_bytes);
        set_resource_limits(owner, personal_ram_bytes, net_cpu_weight.amount, net_cpu_weight.amount);

        // Sysio reslimit
        reslimit_t sysioreslimit(get_self());
        auto sysio_res_key = reslimit_key{sysio_account.value};
        auto sysio_res = sysioreslimit.get(sysio_res_key, "sysio reslimit does not exist.");

        uint64_t sysio_bytes = sysio_allocation.amount * state.bytes_per_unit;
        sysioreslimit.modify(get_self(), sysio_res_key, [&](auto& row) {
            row.ram_bytes += sysio_bytes;
        });

        // Re-read to get updated value for set_resource_limits
        sysio_res = sysioreslimit.get(sysio_res_key);

        // Update the RAM allocation, sysio is a system account so -1, -1 for net and cpu to maintain unlimited.
        set_resource_limits(sysio_account, sysio_res.ram_bytes, -1, -1);

        // Finally, record the node owner entry with the new fields
        nodeowners.emplace(get_self(), node_key, roa::nodeowners{
            .owner = owner,
            .tier = tier,
            .total_sys = total_sys_allocation,
            .allocated_sys = allocated_sys,
            .allocated_bw = allocated_bw,
            .allocated_ram = allocated_ram,
            .network_gen = state.network_gen,
        });

        // Register the node owner with sysio.system emissions. Guarded on
        // emitcfg_t::exists() so that bootstrap paths which do not deploy
        // sysio.system (loadSystemContract=False in Cluster.py) or which run
        // forcereg before setemitcfg continue to work -- the emissions
        // distribution row is simply skipped in that case.
        sysiosystem::emissions::emitcfg_t emitcfg("sysio"_n);
        if (emitcfg.exists()) {
            action(
               {get_self(), "active"_n},
               "sysio"_n,
               "addnodeowner"_n,
               std::make_tuple(owner, tier)
            ).send();
        }

        // TODO: Notify Council contract if needed
    };


    asset roa::get_allocation_for_tier(uint8_t tier) {
        // Retrieve the current roastate
        roastate_t roastate(get_self());
        auto state = roastate.get();

        // Ensure the contract is active
        check(state.is_active, "Contract not active yet.");

        int64_t total_amount = state.total_sys.amount;
        int64_t allocation_amount = 0;

        switch (tier) {
            case 1:
                // T1 fraction: 4/100
                // Add 50 to round to nearest integer
                allocation_amount = (total_amount * 4 + 50) / 100;
                break;
            case 2:
                // T2 fraction: 15/10,000 = 0.0015
                // Add 5,000 to round
                allocation_amount = (total_amount * 15 + 5000) / 10000;
                break;
            case 3:
                // T3 fraction: 3/100,000 = 0.00003
                // Add 50,000 to round
                allocation_amount = (total_amount * 3 + 50000) / 100000;
                break;
            default:
                check(false, "Invalid tier");
        }

        return asset(allocation_amount, state.total_sys.symbol);
    };

    name roa::newuser(const name& creator, const name& nonce, const public_key& pubkey) {
       require_auth(creator);

       roastate_t roastate(get_self());
       auto state = roastate.get();
       check(state.is_active, "ROA is not active yet");

       // Check if creator is a tier-1 node owner
       nodeowners_t nodeowners(get_self(), state.network_gen);
       auto node_key = nodeowner_key{creator.value};
       check(nodeowners.contains(node_key), "Creator is not a registered tier-1 node owner");
       auto node = nodeowners.get(node_key);
       check(node.tier == 1, "Creator is not a registered tier-1 node owner");

        // Check if nonce already exists in sponsors table for this creator
        sponsors_t sponsors(get_self(), creator.value);
        auto sp_key = sponsor_key{nonce.value};
        check(!sponsors.contains(sp_key), "Sponsor entry for this nonce already exists");

        // Build the sub-account name as "<prefix>.<generated>", where the prefix is the creator's
        // (tier-1 owner's) own name — e.g. owner "acme" gets sub-accounts "acme.<random>". This
        // flips the older "<generated>.<suffix>" construction. ROA is privileged, so it can mint
        // any name regardless of Antelope's suffix-namespace ownership rule.
        std::string prefix_str = creator.to_string();
        size_t prefix_len = prefix_str.size();

        const size_t NAME_LENGTH = 12;
        // Need room for "<prefix>." plus at least one generated char.
        check(prefix_len + 2 <= NAME_LENGTH, "Creator name is too long to generate a sub-account under it");
        size_t gen_len = NAME_LENGTH - prefix_len - 1; // chars after "<prefix>."

        // Try up to 3 times to generate a unique username
        name new_username;
        bool created = false;
        uint32_t block_num = current_block_number();

        // Create the username string buffer once, so we don't reallocate
        char uname_str[NAME_LENGTH + 1];
        uname_str[NAME_LENGTH] = 0; // ensure null-termination

        // Pre-fill the buffer with "<prefix>."
        std::memcpy(uname_str, prefix_str.c_str(), prefix_len);
        uname_str[prefix_len] = '.';

        static constexpr char charmap[] = {'1','2','3','4','5',
           'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o',
           'p','q','r','s','t','u','v','w','x','y','z'};
        constexpr size_t charmap_len = sizeof(charmap) / sizeof(charmap[0]);

        // Cheap pseudo-random generator: a splitmix64 finalizer over nonce/attempt/block_num. No
        // crypto is needed here — uniqueness is enforced by the is_account retry below; we only
        // need variation — so this avoids a sha256 intrinsic call per attempt.
        auto mix = [](uint64_t z) {
            z += 0x9E3779B97F4A7C15ULL;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            return z ^ (z >> 31);
        };

        for (uint8_t attempt = 0; attempt < 3; ++attempt) {
            uint64_t x = nonce.value ^ (static_cast<uint64_t>(block_num) << 32)
                         ^ (static_cast<uint64_t>(attempt) * 0x9E3779B97F4A7C15ULL);

            // Fill the generated portion after "<prefix>."
            for (size_t i = 0; i < gen_len; ++i) {
                x = mix(x);
                uname_str[prefix_len + 1 + i] = charmap[x % charmap_len];
            }

            // `uname_str` now holds "<prefix>.<generated>"
            new_username = name(uname_str);

            if (!is_account(new_username)) {
                created = true;
                break;
            }
        }
        check(created, "Failed to generate a unique account name after 3 attempts");

        auto owner_auth = sysiosystem::authority{1, {{pubkey, 1}}, {}};
        auto active_auth = sysiosystem::authority{1, {{pubkey, 1}}, {}};
        action(
            permission_level{get_self(), "active"_n},
            "sysio"_n, "newaccount"_n,
            std::make_tuple( get_self(), new_username, owner_auth, active_auth)
        ).send();

        // Record sponsor mapping
        sponsors.emplace(creator, sp_key, sponsor{
            .nonce = nonce,
            .username = new_username,
        });

        // Update sponsor count for creator
        sponsorcount_t sponsorcount(get_self(), state.network_gen);
        auto sc_key = sponsorcount_key{creator.value};
        sponsorcount.upsert(get_self(), sc_key,
            roa::sponsorcount{.owner = creator, .count = 1},
            [&](auto& row) { row.count += 1; });

        // Update roa policy for sys_account for tracking RAM provided to accounts for account creation
        // It is not expected that sysio.acct will use the RAM. Instead sysio.acct is a placeholder for
        // all RAM provided to individual accounts for the account creation. See sysio.system newaccount.
        auto sys_symbol = state.total_sys.symbol;
        int64_t ram_weight_amount = sysiosystem::newaccount_ram / state.bytes_per_unit;
        policies_t policies(get_self(), "sysio"_n.value);
        auto pol_key = policy_key{"sysio.acct"_n.value};
        policies.get(pol_key, "Missing sysio.acct policy");
        policies.modify(get_self(), pol_key, [&](auto& row) {
            row.ram_weight.amount += ram_weight_amount;
        });

        // Update reslimit for sysio.acct (increase) and sysio (decrease) for the ram
        increase_reslimit("sysio.acct"_n, {0, sys_symbol}, {0, sys_symbol}, sysiosystem::newaccount_ram, true);
        decrease_reslimit("sysio"_n, sysiosystem::newaccount_ram);

        return new_username;
    }

    void roa::newnameduser(const name& account, const public_key& pubkey, uint8_t tier) {
        // Dispatched by the OPP depot (sysio.msgch) in the NFT node-owner claim flow, the same way
        // as nodeownreg: msgch sends this inline declaring {sysio.roa, active}, accepted via the
        // msgch@sysio.code delegation on sysio.roa.active wired at bootstrap.
        require_auth(get_self());

        roastate_t roastate(get_self());
        auto state = roastate.get();
        check(state.is_active, "ROA is not active yet");

        // Idempotent: if the account already exists, the create step is already done. A key or tier
        // mismatch on a pre-existing account is caught downstream by nodeownreg's soft-fail.
        if (is_account(account)) return;

        // Tier is a depot/system invariant -- a bad value means the dispatch is malformed, so abort
        // (the name charset [a-z1-5.] is already enforced by the name type).
        check(tier >= 1 && tier <= 3, "Tier level must be between 1 and 3");
        // The chosen name is claim-payload: if it violates the tier's length rule, skip creation
        // (non-throwing) and let nodeownreg soft-fail with NAME_INVALID. Without this guard a bad
        // name would either abort the depot dispatch or create an account the claim then rejects.
        if (!valid_name_for_tier(account, tier)) return;

        // Create the account with the holder's key as both owner and active.
        auto auth = sysiosystem::authority{1, {{pubkey, 1}}, {}};
        action(permission_level{get_self(), "active"_n}, "sysio"_n, "newaccount"_n,
               std::make_tuple(get_self(), account, auth, auth)).send();

        // Fund the fixed newaccount_ram from sysio's pool (same model as newuser): bucket the RAM
        // under sysio.acct and draw it from sysio. The inline newaccount's transfer_ram (sysio.system)
        // moves the matching chain quota from sysio to the new account.
        auto sys_symbol = state.total_sys.symbol;
        int64_t ram_weight_amount = sysiosystem::newaccount_ram / state.bytes_per_unit;
        policies_t policies(get_self(), "sysio"_n.value);
        auto pol_key = policy_key{"sysio.acct"_n.value};
        policies.get(pol_key, "Missing sysio.acct policy");
        policies.modify(get_self(), pol_key, [&](auto& row) {
            row.ram_weight.amount += ram_weight_amount;
        });
        increase_reslimit("sysio.acct"_n, {0, sys_symbol}, {0, sys_symbol}, sysiosystem::newaccount_ram, true);
        decrease_reslimit("sysio"_n, sysiosystem::newaccount_ram);
    }
};

// namespace roa
