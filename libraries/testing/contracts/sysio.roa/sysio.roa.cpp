#include "sysio.roa.hpp"

namespace sysio {

    void roa::activateroa(const asset& total_sys, const uint64_t& bytes_per_unit) {
        require_auth(get_self());

        roastate_t roastate(get_self(), get_self().value);
        auto state = roastate.get_or_default();

        check(!state.is_active, "Contract already activated.");

        state.is_active = true;
        state.total_sys = total_sys;
        state.bytes_per_unit = bytes_per_unit;
        state.network_gen = 0;
        roastate.set(state, get_self());

        const int64_t total_amount = total_sys.amount; // smallest units

        // Fractions per node (rational approach):
        // T1: 4% = 4/100 (add 50 for rounding)
        int64_t t1_per_node = (total_amount * 4 + 50) / 100;
        int64_t t1_total = t1_per_node * 21;

        // T2: 0.0015 = 15/10,000 (add 5,000 for rounding)
        int64_t t2_per_node = (total_amount * 15 + 5000) / 10000;
        int64_t t2_total = t2_per_node * 84;

        // T3: 0.00003 = 3/100,000 (add 50,000 for rounding)
        int64_t t3_per_node = (total_amount * 3 + 50000) / 100000;
        int64_t t3_total = t3_per_node * 1000;

        // Allocated sum
        int64_t allocated = t1_total + t2_total + t3_total;

        // Leftover
        int64_t leftover = total_amount - allocated;

        // Split leftover in half
        int64_t half_leftover = leftover / 2;
        int64_t other_half = leftover - half_leftover; // ensures exact sum

        // Convert to bytes using bytes_per_unit = bytes per smallest unit
        uint64_t roa_ram_bytes = (uint64_t)(half_leftover * bytes_per_unit);
        uint64_t sysio_ram_bytes = (uint64_t)(other_half * bytes_per_unit);

        // Create/Update reslimit for sysio.roa (self)
        reslimit_t roareslimit(get_self(), get_self().value);
        check(roareslimit.find(get_self().value) == roareslimit.end(), "roa reslimit already exists.");
        roareslimit.emplace(get_self(), [&](auto& row) {
            row.owner = get_self();
            row.net_weight = asset(0, total_sys.symbol);
            row.cpu_weight = asset(0, total_sys.symbol);
            row.ram_bytes = roa_ram_bytes;
        });

        // Set sysio.roas new account limits.
        set_resource_limits(get_self(), roa_ram_bytes, -1, -1);

        // Create/Update reslimit for sysio
        name sys_account = "sysio"_n;
        reslimit_t sysioreslimit(get_self(), sys_account.value);
        check(sysioreslimit.find(sys_account.value) == sysioreslimit.end(), "sysio reslimit already exists.");
        sysioreslimit.emplace(get_self(), [&](auto& row) {
            row.owner = sys_account;
            row.net_weight = asset(0, total_sys.symbol);
            row.cpu_weight = asset(0, total_sys.symbol);
            row.ram_bytes = sysio_ram_bytes;
        });

        // Set sysio.roas new account limits.
        set_resource_limits(sys_account, sysio_ram_bytes, -1, -1);
    };

    void roa::setbyteprice(const uint64_t& bytes_per_unit) {
        require_auth(get_self());

        //Singelton index
        roastate_t roastate(get_self(), get_self().value);

        // Gets values in the table.
        auto state = roastate.get();

        // Make sure ROA 'is_active' first.
        check(state.is_active, "ROA is not currently active");
        
        state.bytes_per_unit = bytes_per_unit;

        // Set values to table.
        roastate.set(state, get_self());
    };
    
    void roa::regnodeowner(const name& owner, const uint8_t& tier) {
        require_auth(get_self());

        roastate_t roastate(get_self(), get_self().value);
        auto state = roastate.get();
        check(state.is_active, "ROA is not active yet");

        nodeowners_t nodeowners(get_self(), state.network_gen);
        auto node_itr = nodeowners.find(owner.value);
        check(node_itr == nodeowners.end(), "This account is already registered.");

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
        auto pol_itr = policies.find(owner.value);

        name sysio_account = "sysio"_n;
        asset zero_asset(0, state.total_sys.symbol);

        if (pol_itr == policies.end()) {
            // Create personal policy
            policies.emplace(get_self(), [&](auto& row) {
                row.owner = owner;
                row.issuer = owner;
                row.net_weight = net_cpu_weight; 
                row.cpu_weight = net_cpu_weight; 
                row.ram_weight = personal_ram_weight; 
                row.bytes_per_unit = state.bytes_per_unit;
                row.time_block = 1; 
            });

            // Create sysio policy for RAM
            policies.emplace(get_self(), [&](auto& row) {
                row.owner = sysio_account;
                row.issuer = owner;
                row.net_weight = zero_asset;
                row.cpu_weight = zero_asset;
                row.ram_weight = sysio_allocation; 
                row.bytes_per_unit = state.bytes_per_unit;
                row.time_block = 1;
            });
        }

        // Owner reslimits
        reslimit_t reslimits(get_self(), owner.value);
        auto res_itr = reslimits.find(owner.value);

        if (res_itr == reslimits.end()) {
            reslimits.emplace(get_self(), [&](auto& row) {
                row.owner = owner;
                row.net_weight = net_cpu_weight;
                row.cpu_weight = net_cpu_weight;
                row.ram_bytes = personal_ram_bytes;
            });
            set_resource_limits(owner, personal_ram_bytes, net_cpu_weight.amount, net_cpu_weight.amount);
        } else {
            reslimits.modify(res_itr, get_self(), [&](auto& row) {
                row.net_weight += net_cpu_weight;
                row.cpu_weight += net_cpu_weight;
                row.ram_bytes += personal_ram_bytes;
            });
            set_resource_limits(owner, res_itr->ram_bytes, res_itr->net_weight.amount, res_itr->cpu_weight.amount);
        }

        // Sysio reslimit
        reslimit_t sysioreslimit(get_self(), sysio_account.value);
        auto sysio_res_itr = sysioreslimit.find(sysio_account.value);
        check(sysio_res_itr != sysioreslimit.end(), "sysio reslimit does not exist.");

        uint64_t sysio_bytes = sysio_allocation.amount * state.bytes_per_unit;
        sysioreslimit.modify(sysio_res_itr, get_self(), [&](auto& row) {
            row.ram_bytes += sysio_bytes;
        });

        // Update the RAM allocation, sysio is a system account so -1, -1 for net and cpu to maintain unlimited.
        set_resource_limits(sysio_account, sysio_res_itr->ram_bytes, -1, -1);

        // Finally, record the node owner entry with the new fields
        nodeowners.emplace(get_self(), [&](auto& row) {
            row.owner = owner;
            row.tier = tier;
            row.total_sys = total_sys_allocation;
            row.allocated_sys = allocated_sys;
            row.allocated_bw = allocated_bw;
            row.allocated_ram = allocated_ram;
            row.network_gen = state.network_gen;
        });

        // TODO: Notify Council contract if needed
    };

    void roa::addpolicy(const name& owner, const name& issuer, const asset& net_weight, const asset& cpu_weight, const asset& ram_weight, const uint32_t& time_block, const uint8_t& network_gen) {
        // Can only issue policies if you are the issuer.
        require_auth(issuer);

        // Ensure ROA is active
        roastate_t roastate(get_self(), get_self().value);
        auto state = roastate.get();
        check(state.is_active, "ROA is not currently active");
        check(network_gen <= state.network_gen, "Invalid network generation.");

        // Ensure issuer is a node owner for the specified network_gen
        nodeowners_t nodeowners(get_self(), network_gen);
        auto node_itr = nodeowners.find(issuer.value);
        check(node_itr != nodeowners.end(), "Only Node Owners can issue policies for this generation.");

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
        asset free_sys = node_itr->total_sys - node_itr->allocated_sys;
        check(total_new_allocation.amount <= free_sys.amount, "Not enough unallocated SYS for this policy.");

        // Calculate RAM bytes
        int64_t ram_bytes_to_allocate = (int64_t)ram_weight.amount * (int64_t)state.bytes_per_unit;

        // Check if a policy already exists for this owner from this issuer
        policies_t policies(get_self(), issuer.value);
        auto pol_iter = policies.find(owner.value);
        check(pol_iter == policies.end(), "A policy for this owner already exists from this issuer. Use expandpolicy instead.");

        // Update reslimit for the 'owner'
        reslimit_t reslimit(get_self(), owner.value);
        auto res_itr = reslimit.find(owner.value);

        if (res_itr == reslimit.end()) {
            reslimit.emplace(get_self(), [&](auto& row) {
                row.owner = owner;
                row.net_weight = sysio_acct ? asset(0, state.total_sys.symbol) : net_weight;
                row.cpu_weight = sysio_acct ? asset(0, state.total_sys.symbol) : cpu_weight;
                row.ram_bytes = (uint64_t)ram_bytes_to_allocate;
            });
        } else {
            reslimit.modify(res_itr, get_self(), [&](auto& row) {
                if (!sysio_acct) {
                    row.net_weight.amount += net_weight.amount;
                    row.cpu_weight.amount += cpu_weight.amount;
                }
                row.ram_bytes += (uint64_t)ram_bytes_to_allocate;
            });
        }

        // After updating resource limits, set the system resource limits
        auto updated_res_itr = reslimit.find(owner.value);
        if (sysio_acct) {
            // Infinite CPU/NET for sysio accounts
            set_resource_limits(owner, updated_res_itr->ram_bytes, -1, -1);
        } else {
            set_resource_limits(owner, updated_res_itr->ram_bytes, updated_res_itr->net_weight.amount, updated_res_itr->cpu_weight.amount);
        }

        // Add the new policy
        policies.emplace(get_self(), [&](auto& row) {
            row.owner = owner;
            row.issuer = issuer;
            row.net_weight = net_weight;
            row.cpu_weight = cpu_weight;
            row.ram_weight = ram_weight;
            row.bytes_per_unit = state.bytes_per_unit;
            row.time_block = time_block;
        });

        // Update the issuer's allocations
        nodeowners.modify(node_itr, get_self(), [&](auto& row) {
            row.allocated_sys.amount += total_new_allocation.amount;
            row.allocated_bw.amount += (net_weight.amount + cpu_weight.amount);
            row.allocated_ram.amount += ram_weight.amount;
        });
    };

    void roa::expandpolicy(const name& owner, const name& issuer, const asset& net_weight, const asset& cpu_weight, const asset& ram_weight, const uint8_t& network_gen) {
        require_auth(issuer);

        // Ensure issuer is a node owner in the given generation
        nodeowners_t nodeowners(get_self(), network_gen);
        auto node_itr = nodeowners.find(issuer.value);
        check(node_itr != nodeowners.end(), "Only Node Owners can manage policies.");

        // Fetch issuer's policies scoped by issuer
        policies_t policies(get_self(), issuer.value);
        auto pol_itr = policies.find(owner.value);
        check(pol_itr != policies.end(), "You have no policy for this owner.");

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
        asset free_sys = node_itr->total_sys - node_itr->allocated_sys;
        check(total_new_allocation.amount <= free_sys.amount, "Issuer does not have enough unallocated SYS for this policy expansion.");

        // Convert RAM weight to bytes
        int64_t ram_bytes_to_allocate = (int64_t)ram_weight.amount * (int64_t)pol_itr->bytes_per_unit;

        // Update the policy weights
        policies.modify(pol_itr, get_self(), [&](auto& row) {
            if (!sysio_acct) {
                row.net_weight.amount += net_weight.amount;
                row.cpu_weight.amount += cpu_weight.amount;
            }
            row.ram_weight.amount += ram_weight.amount;
        });

        // Update issuer's allocations in nodeowners
        nodeowners.modify(node_itr, get_self(), [&](auto& row) {
            row.allocated_sys.amount += total_new_allocation.amount;
            row.allocated_bw.amount += (net_weight.amount + cpu_weight.amount);
            row.allocated_ram.amount += ram_weight.amount;
        });

        // Update owner's resource limits
        reslimit_t reslimit(get_self(), owner.value);
        auto res_itr = reslimit.find(owner.value);
        check(res_itr != reslimit.end(), "No reslimit found for user.");

        reslimit.modify(res_itr, get_self(), [&](auto& row) {
            if (!sysio_acct) {
                row.net_weight.amount += net_weight.amount;
                row.cpu_weight.amount += cpu_weight.amount;
            }
            row.ram_bytes += (uint64_t)ram_bytes_to_allocate;
        });

        auto updated_res_itr = reslimit.find(owner.value);
        if (sysio_acct) {
            // sysio accounts remain infinite CPU/NET
            set_resource_limits(owner, updated_res_itr->ram_bytes, -1, -1);
        } else {
            set_resource_limits(owner, updated_res_itr->ram_bytes, updated_res_itr->net_weight.amount, updated_res_itr->cpu_weight.amount);
        }
    };

    void roa::extendpolicy(const name& owner, const name& issuer, const uint32_t& new_time_block) {
        require_auth(issuer);

        // Pointer to Policies table
        policies_t policies(get_self(), issuer.value);
        auto pol_itr = policies.find(owner.value);

        check(pol_itr != policies.end(), "Policy does not exist under this issuer for this owner");
        check(new_time_block > pol_itr->time_block, "Cannot reduce a policies existing time_block");
        check(new_time_block > current_block_number(), "You cannot set a time_block lower than the current block");

        policies.modify(pol_itr, get_self(), [&](auto& row) {
            row.time_block = new_time_block;
        });
    };
    
    void roa::reducepolicy(const name& owner, const name& issuer, const asset& net_weight, const asset& cpu_weight, const asset& ram_weight, const uint8_t& network_gen) { 
        // ---- Native Action, see chain > sysio_contract.cpp in core ---- 
    };

    // ---- Private Helper Function ----
    asset roa::get_allocation_for_tier(uint8_t tier) {
        // Retrieve the current roastate
        roastate_t roastate(get_self(), get_self().value);
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
    }
} // namespace roa

// SYSIO_DISPATCH(sysio::roa, (reducepolicy));
