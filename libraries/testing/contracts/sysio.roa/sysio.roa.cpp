#include "sysio.roa.hpp"
#include "native.hpp"

namespace sysio {

    bool is_sysio_account(const name& account) {
        return account.prefix() == "sysio"_n;
    }

    roa::resources_t roa::update_reslimit(const name& owner, const asset& netWeight, const asset& cpuWeight, int64_t ram_bytes) {
        bool sysio_acct = is_sysio_account(owner);

        reslimit_t reslimit(get_self(), get_self().value);
        auto res_itr = reslimit.find(owner.value);

        if (res_itr == reslimit.end()) {
            resources_t res = {
                .net = sysio_acct ? asset(0, netWeight.symbol) : netWeight,
                .cpu = sysio_acct ? asset(0, cpuWeight.symbol) : cpuWeight,
                .ram_bytes = (uint64_t)ram_bytes
            };
            reslimit.emplace(get_self(), [&](auto& row) {
                row.owner = owner;
                row.net_weight = res.net;
                row.cpu_weight = res.cpu;
                row.ram_bytes = res.ram_bytes;
            });
            return res;
        } else {
            reslimit.modify(res_itr, get_self(), [&](auto& row) {
                if (!sysio_acct) {
                    row.net_weight.amount += netWeight.amount;
                    row.cpu_weight.amount += cpuWeight.amount;
                }
                row.ram_bytes += (uint64_t)ram_bytes;
            });
            return resources_t{
                .net = res_itr->net_weight,
                .cpu = res_itr->cpu_weight,
                .ram_bytes = res_itr->ram_bytes
            };
        }
    }

    void roa::activateroa(const asset& totalSys, const uint64_t& bytesPerUnit) {
        require_auth(get_self());

        roastate_t roastate(get_self(), get_self().value);
        auto state = roastate.get_or_default();

        check(!state.is_active, "Contract already activated.");
        check(totalSys.symbol == symbol("SYS", 4), "Total SYS must be SYS.");

        state.is_active = true;
        state.total_sys = totalSys;
        state.bytes_per_unit = bytesPerUnit;
        state.network_gen = 0;
        roastate.set(state, get_self());

        const int64_t total_amount = totalSys.amount; // smallest units

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
        uint64_t roa_ram_bytes = (uint64_t)(half_leftover * bytesPerUnit);
        uint64_t sysio_ram_bytes = (uint64_t)(other_half * bytesPerUnit);

        // Create/Update reslimit for sysio.roa (self)
        update_reslimit(get_self(), asset(0, totalSys.symbol), asset(0, totalSys.symbol), roa_ram_bytes);

        // Set sysio.roas new account limits.
        set_resource_limits(get_self(), roa_ram_bytes, -1, -1);

        // Create/Update reslimit for sysio
        name sys_account = "sysio"_n;
        update_reslimit(sys_account, asset(0, totalSys.symbol), asset(0, totalSys.symbol), sysio_ram_bytes);

        // Set sysio new account limits.
        set_resource_limits(sys_account, sysio_ram_bytes, -1, -1);

        // Add policy for sys_account for tracking RAM provided to accounts for account creation
        policies_t policies(get_self(), sys_account.value);
        policies.emplace(get_self(), [&](auto& row) {
            row.owner = "sysio.acct"_n;
            row.issuer = sys_account;
            row.net_weight = asset(0, totalSys.symbol);
            row.cpu_weight = asset(0, totalSys.symbol);
            row.ram_weight = asset(0, totalSys.symbol);
            row.bytes_per_unit = state.bytes_per_unit;
            row.time_block = 0;
        });
        // Provide RAM for sysio.acct itself, but provide no CPU/NET
        update_reslimit("sysio.acct"_n, asset(0, totalSys.symbol), asset(0, totalSys.symbol), sysiosystem::newaccount_ram);
        set_resource_limits("sysio.acct"_n, sysiosystem::newaccount_ram, 0, 0);
    };

    void roa::setbyteprice(const uint64_t& bytesPerUnit) {
        require_auth(get_self());

        //Singelton index
        roastate_t roastate(get_self(), get_self().value);

        // Gets values in the table.
        auto state = roastate.get();

        // Make sure ROA 'is_active' first.
        check(state.is_active, "ROA is not currently active");
        check(sysiosystem::newaccount_ram == (sysiosystem::newaccount_ram / bytesPerUnit) * bytesPerUnit,
              "newaccount_ram needs to be evenly divisable to avoid dust");
        
        state.bytes_per_unit = bytesPerUnit;

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

    void roa::addpolicy(const name& owner, const name& issuer, const asset& netWeight, const asset& cpuWeight, const asset& ramWeight, const uint32_t& timeBlock, const uint8_t& networkGen) {
        // Can only issue policies if you are the issuer.
        require_auth(issuer);

        // Ensure ROA is active
        roastate_t roastate(get_self(), get_self().value);
        auto state = roastate.get();
        check(state.is_active, "ROA is not currently active");
        check(networkGen <= state.network_gen, "Invalid network generation.");

        // Ensure issuer is a node owner for the specified network_gen
        nodeowners_t nodeowners(get_self(), networkGen);
        auto node_itr = nodeowners.find(issuer.value);
        check(node_itr != nodeowners.end(), "Only Node Owners can issue policies for this generation.");

        // Validate weights
        check(netWeight.amount >= 0, "NET weight cannot be negative");
        check(cpuWeight.amount >= 0, "CPU weight cannot be negative");
        check(ramWeight.amount >= 0, "RAM weight cannot be negative");

        // If owner is sysio or sysio.* account, no CPU/NET allowed
        bool sysio_acct = is_sysio_account(owner);
        if (sysio_acct) {
            check(netWeight.amount == 0 && cpuWeight.amount == 0, "Cannot allocate CPU/NET to sysio accounts.");
        }

        // Check that at least one is non-zero
        check(!(netWeight.amount == 0 && cpuWeight.amount == 0 && ramWeight.amount == 0), "At least one of NET, CPU, or RAM must be allocated.");

        // Total new allocation
        asset total_new_allocation = netWeight + cpuWeight + ramWeight;

        // Ensure issuer has enough unallocated SYS
        asset free_sys = node_itr->total_sys - node_itr->allocated_sys;
        check(total_new_allocation.amount <= free_sys.amount, "Not enough unallocated SYS for this policy.");

        // Calculate RAM bytes
        int64_t ram_bytes_to_allocate = (int64_t)ramWeight.amount * (int64_t)state.bytes_per_unit;

        // Check if a policy already exists for this owner from this issuer
        policies_t policies(get_self(), issuer.value);
        auto pol_iter = policies.find(owner.value);
        check(pol_iter == policies.end(), "A policy for this owner already exists from this issuer. Use expandpolicy instead.");

        // Update reslimit for the 'owner'
        update_reslimit(owner, netWeight, cpuWeight, ram_bytes_to_allocate);
        // Update the system resource limits
        add_system_resources(owner, netWeight.amount, cpuWeight.amount, ram_bytes_to_allocate);

        // Add the new policy
        policies.emplace(get_self(), [&](auto& row) {
            row.owner = owner;
            row.issuer = issuer;
            row.net_weight = netWeight;
            row.cpu_weight = cpuWeight;
            row.ram_weight = ramWeight;
            row.bytes_per_unit = state.bytes_per_unit;
            row.time_block = timeBlock;
        });

        // Update the issuer's allocations
        nodeowners.modify(node_itr, get_self(), [&](auto& row) {
            row.allocated_sys.amount += total_new_allocation.amount;
            row.allocated_bw.amount += (netWeight.amount + cpuWeight.amount);
            row.allocated_ram.amount += ramWeight.amount;
        });
    };

    void roa::expandpolicy(const name& owner, const name& issuer, const asset& netWeight, const asset& cpuWeight, const asset& ramWeight, const uint8_t& networkGen) {
        require_auth(issuer);

        // Ensure issuer is a node owner in the given generation
        nodeowners_t nodeowners(get_self(), networkGen);
        auto node_itr = nodeowners.find(issuer.value);
        check(node_itr != nodeowners.end(), "Only Node Owners can manage policies.");

        // Fetch issuer's policies scoped by issuer
        policies_t policies(get_self(), issuer.value);
        auto pol_itr = policies.find(owner.value);
        check(pol_itr != policies.end(), "You have no policy for this owner.");

        // Validate weights (zero increments allowed)
        check(netWeight.amount >= 0, "NET weight cannot be negative");
        check(cpuWeight.amount >= 0, "CPU weight cannot be negative");
        check(ramWeight.amount >= 0, "RAM weight cannot be negative");

        // Check that at least one of them is non-zero
        if (netWeight.amount == 0 && cpuWeight.amount == 0 && ramWeight.amount == 0) {
            check(false, "At least one of NET, CPU, or RAM must be increased.");
        }

        bool sysio_acct = is_sysio_account(owner);
        if (sysio_acct) {
            // Cannot allocate CPU/NET to sysio accounts
            check(netWeight.amount == 0 && cpuWeight.amount == 0, "Cannot allocate CPU/NET to sysio accounts.");
        }

        // Calculate total new allocation
        asset total_new_allocation = netWeight + cpuWeight + ramWeight;

        // Ensure the issuer has enough unallocated SYS
        asset free_sys = node_itr->total_sys - node_itr->allocated_sys;
        check(total_new_allocation.amount <= free_sys.amount, "Issuer does not have enough unallocated SYS for this policy expansion.");

        // Convert RAM weight to bytes
        int64_t ram_bytes_to_allocate = (int64_t)ramWeight.amount * (int64_t)pol_itr->bytes_per_unit;

        // Update the policy weights
        policies.modify(pol_itr, get_self(), [&](auto& row) {
            if (!sysio_acct) {
                row.net_weight.amount += netWeight.amount;
                row.cpu_weight.amount += cpuWeight.amount;
            }
            row.ram_weight.amount += ramWeight.amount;
        });

        // Update issuer's allocations in nodeowners
        nodeowners.modify(node_itr, get_self(), [&](auto& row) {
            row.allocated_sys.amount += total_new_allocation.amount;
            row.allocated_bw.amount += (netWeight.amount + cpuWeight.amount);
            row.allocated_ram.amount += ramWeight.amount;
        });

        // Update owner's resource limits
        update_reslimit(owner, netWeight, cpuWeight, ram_bytes_to_allocate);
        // Update the system resource limits
        add_system_resources(owner, netWeight.amount, cpuWeight.amount, ram_bytes_to_allocate);
    };

    void roa::extendpolicy(const name& owner, const name& issuer, const uint32_t& newTimeBlock) {
        require_auth(issuer);

        // Pointer to Policies table
        policies_t policies(get_self(), issuer.value);
        auto pol_itr = policies.find(owner.value);

        check(pol_itr != policies.end(), "Policy does not exist under this issuer for this owner");
        check(newTimeBlock > pol_itr->time_block, "Cannot reduce a policies existing time_block");
        check(newTimeBlock > current_block_number(), "You cannot set a time_block lower than the current block");

        policies.modify(pol_itr, get_self(), [&](auto& row) {
            row.time_block = newTimeBlock;
        });
    };
    
    void roa::reducepolicy(const name& owner, const name& issuer, const asset& net_weight, const asset& cpu_weight, const asset& ram_weight, const uint8_t& networkGen) {
        require_auth(issuer);

        // Ensure issuer is a node owner in the given generation
        nodeowners_t nodeowners(get_self(), networkGen);
        auto node_itr = nodeowners.find(issuer.value);
        check(node_itr != nodeowners.end(), "Only Node Owners can manage policies.");

        // Fetch issuer's policies scoped by issuer
        policies_t policies(get_self(), issuer.value);
        auto pol_itr = policies.find(owner.value);
        check(pol_itr != policies.end(), "You have no policy for this owner.");

        auto pol_row = *pol_itr;

        // Validate time block
        uint32_t current_block = current_block_number();
        check(current_block >= pol_row.time_block, "Cannot reduce policy before time_block");

        // Ensure we don't reduce below zero
        check(net_weight.amount <= pol_row.net_weight.amount, "Cannot reduce NET below zero");
        check(cpu_weight.amount <= pol_row.cpu_weight.amount, "Cannot reduce CPU below zero");
        check(ram_weight.amount <= pol_row.ram_weight.amount, "Cannot reduce RAM below zero");

        // Special sysio check
        bool sysio_acct = is_sysio_account(owner);
        if (sysio_acct && pol_row.time_block == 1 && pol_row.issuer == issuer) {
            check(false, "Cannot reduce the sysio policies created at node registration");
        }

        reslimit_t reslimit(get_self(), get_self().value);
        auto res_itr = reslimit.find(owner.value);
        check(res_itr != reslimit.end(), "reslimit row does not exist for this owner");
        auto rl_row = *res_itr;

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

        set_resource_limits(
            owner,
            ram_bytes - divisible_ram_to_reclaim,
            net_limit - net_weight.amount,
            cpu_limit - cpu_weight.amount
        );

        // Update reslimit row
        reslimit.modify(res_itr, get_self(), [&](auto& row) {
            if (!sysio_acct) {
                row.net_weight.amount += net_weight.amount;
                row.cpu_weight.amount += cpu_weight.amount;
            }
            row.ram_bytes += static_cast<uint64_t>(rl_row.ram_bytes - divisible_ram_to_reclaim);;
        });

        // Update / remove the policies row
        pol_row.net_weight -= net_weight;
        pol_row.cpu_weight -= cpu_weight;
        pol_row.ram_weight -= reclaimed_ram_weight;

        bool all_zero = (pol_row.net_weight.amount == 0 &&
                         pol_row.cpu_weight.amount == 0 &&
                         pol_row.ram_weight.amount == 0);
        if (all_zero) {
            policies.erase(pol_itr);
        } else {
            policies.modify(pol_itr, get_self(), [&](auto& row) {
                if (!sysio_acct) {
                    row.net_weight.amount = pol_row.net_weight.amount;
                    row.cpu_weight.amount = pol_row.cpu_weight.amount;
                }
                row.ram_weight.amount = pol_row.ram_weight.amount;
            });
        }

        // Update nodeowners row
        int64_t total_reclaimed_sys_amount = net_weight.amount + cpu_weight.amount + reclaimed_ram_weight.amount;
        nodeowners.modify(node_itr, get_self(), [&](auto &row) {
            row.allocated_sys.amount -= total_reclaimed_sys_amount;
            row.allocated_bw.amount -= (net_weight.amount + cpu_weight.amount);
            row.allocated_ram.amount -= reclaimed_ram_weight.amount;
        });
    };

    void roa::initnodereg(const name& owner) {
        
        require_auth(permission_level{owner, "auth.ext"_n});

        roastate_t roastate(get_self(), get_self().value);
        auto state = roastate.get();
        check(state.is_active, "ROA is not active yet");

        nodeowners_t nodeowners(get_self(), state.network_gen);
        auto node_itr = nodeowners.find(owner.value);
        check(node_itr == nodeowners.end(), "This account is already registered.");

        nodeownerreg_t nodereg(get_self(), state.network_gen);
        auto nodereg_itr = nodereg.find(owner.value);
        
        if (nodereg_itr != nodereg.end()) {
            check(nodereg_itr->status == 3, "A registration is already pending or confirmed.");

            nodereg.modify(nodereg_itr,get_self(), [&](auto &row){
                row.status = 0;
                row.trx_id = {};
                row.trx_signature = {};
                row.tier = 0;
                row.block_num = 0;
            });

        } else {
            nodereg.emplace(get_self(), [&](auto &row){
                row.owner = owner;
                row.status = 0;
                row.trx_id = {};
                row.trx_signature = {};
                row.tier = 0;
                row.block_num = 0;
            });
        }
    };

    void roa::setpending(const name& owner, const uint8_t& tier ,const checksum256& trxId, const uint128_t& blockNum, const bytes& sig) {
        
        require_auth(permission_level{owner, "auth.ext"_n});
    
        roastate_t roastate(get_self(), get_self().value);
        auto state = roastate.get();
        check(state.is_active, "ROA is not active yet");

        nodeownerreg_t nodereg(get_self(), state.network_gen);
        auto nodereg_itr = nodereg.find(owner.value);
        
        check(nodereg_itr != nodereg.end(),"Registration not initialized yet");

        check(tier > 0 && tier <= 3 , "Tier level must be between 1 and 3");

        check(nodereg_itr->status == 0, "Registration status must be 0 ( INTENT ) to set PENDING.");


        auto bytrxid_index = nodereg.get_index<"bytrxid"_n>();
        auto foundtrxId = bytrxid_index.find(trxId);
        check(foundtrxId == bytrxid_index.end(),"This trx Id is already used");

        nodereg.modify(nodereg_itr,get_self(),[&](auto &row){
            row.status = 1; 
            row.trx_id = trxId;
            row.trx_signature = sig;
            row.tier = tier; 
            row.block_num = blockNum;
        });
        // TODO: might need to add require_receipient(account_name) call to notify validators of a new pending transaction
        // require_recipient("Validator");
    };

    void roa::finalizereg(const name& owner,const uint8_t& status) {

        // TODO -> Require authorization for validator !!!
        // require_auth();
        
        roastate_t roastate(get_self(), get_self().value);
        auto state = roastate.get();
        check(state.is_active, "ROA is not active yet");


        nodeownerreg_t nodereg(get_self(), state.network_gen);
        auto nodereg_itr = nodereg.find(owner.value);

        check(nodereg_itr != nodereg.end(),"No registration record found !");

        check(nodereg_itr->status == 1, "Registration is not in 1 ( PENDING ) state.");

        check(status == 3 || status == 4, "Invalid status: Can only confirm (2) or reject (3)");

        if(status == 2){
            
            regnodeowner(owner,nodereg_itr->tier);

            nodereg.modify(nodereg_itr,get_self(),[&](auto &row){
                row.status = 2;
            });

            //TODO -> Add require_receipient(account_name) call to notify council contract 
            // require_receipient("Council");

        } else {
            nodereg.modify(nodereg_itr,get_self(),[&](auto &row){
                row.status = 3;
            });
        }
    };

    void roa::forcereg(const name& owner, const uint8_t& tier) {
        require_auth(get_self());

        check(tier > 0 && tier <= 3, "Tier level must be between 1 and 3");

        regnodeowner(owner, tier);
    };

    // ---- Private Helper Function ----

    void roa::regnodeowner(const name& owner, const uint8_t& tier) {

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
        resources_t res = update_reslimit(owner, net_cpu_weight, net_cpu_weight, personal_ram_bytes);
        set_resource_limits(owner, res.ram_bytes, res.net.amount, res.cpu.amount);

        // Sysio reslimit
        reslimit_t sysioreslimit(get_self(), get_self().value);
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
    };

    name roa::newuser(const name& creator, const name& nonce, const public_key& pubkey) {
        require_auth(creator);

       roastate_t roastate(get_self(), get_self().value);
       auto state = roastate.get();
       check(state.is_active, "ROA is not active yet");

       // Check if creator is a tier-1 node owner
       nodeowners_t nodeowners(get_self(), state.network_gen);
       auto node_itr = nodeowners.find(creator.value);
       check(node_itr != nodeowners.end() && node_itr->tier == 1, "Creator is not a registered tier-1 node owner");

        // Check if nonce already exists in sponsors table for this creator
        sponsors_t sponsors(get_self(), creator.value);
        auto sponsor_itr = sponsors.find(nonce.value);
        check(sponsor_itr == sponsors.end(), "Sponsor entry for this nonce already exists");

        // Try up to 3 times to generate a unique username
        name new_username;
        bool created = false;
        uint32_t block_num = current_block_number();

        // Create the username string buffer once, so we don't reallocate
        const size_t NAME_LENGTH = 12; // sysio name constraints
        char uname_str[NAME_LENGTH + 1];
        uname_str[NAME_LENGTH] = 0; // ensure null-termination

        for (uint8_t attempt = 0; attempt < 3; ++attempt) {
            // Hash nonce + attempt + block_num to generate username
            std::string input = nonce.to_string() + std::to_string(attempt) + std::to_string(block_num);
            checksum256 hash = sha256(input.c_str(), input.size());

            static constexpr char charmap[] = {'1','2','3','4','5',
               'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o',
               'p','q','r','s','t','u','v','w','x','y','z'};
            constexpr size_t charmap_len = sizeof(charmap) / sizeof(charmap[0]);

            // Use first 12 chars of hash as account name (sysio name constraints)
            for (size_t i = 0; i < 12; ++i) {
                auto offset = hash.extract_as_byte_array()[i] % charmap_len;
                uname_str[i] = charmap[offset];
            }

            new_username = name(uname_str);

            if (!is_account(new_username)) {
                created = true;
                break;
            }
        }
        check(created, "Failed to generate a unique account name after 3 attempts");

        auto owner_auth = sysiosystem::authority{1, {{pubkey, 1}}, {}, {}};
        auto active_auth = sysiosystem::authority{1, {{pubkey, 1}}, {}, {}};
        action(
            permission_level{get_self(), "active"_n},
            "sysio"_n, "newaccount"_n,
            std::make_tuple( get_self(), new_username, owner_auth, active_auth)
        ).send();

        // Record sponsor mapping
        sponsors.emplace(creator, [&](auto& row) {
            row.nonce = nonce;
            row.username = new_username;
        });

        // Update sponsor count for creator
        sponsorcount_t sponsorcount(get_self(), state.network_gen);
        auto count_itr = sponsorcount.find(creator.value);
        if (count_itr == sponsorcount.end()) {
            sponsorcount.emplace(get_self(), [&](auto& row) {
                row.owner = creator;
                row.count = 1;
            });
        } else {
            sponsorcount.modify(count_itr, get_self(), [&](auto& row) {
                row.count += 1;
            });
        }

        // Update roa policy for sys_account for tracking RAM provided to accounts for account creation
        // It is not expected that sysio.acct will use the RAM. Instead sysio.acct is a placeholder for
        // all RAM provided to individual accounts for the account creation. See sysio.system newaccount.
        auto sys_symbol = state.total_sys.symbol;
        int64_t ram_weight_amount = sysiosystem::newaccount_ram / state.bytes_per_unit;
        policies_t policies(get_self(), "sysio"_n.value);
        auto pol_itr = policies.find("sysio.acct"_n.value);
        check(pol_itr != policies.end(), "Missing sysio.acct policy");
        policies.modify(pol_itr, get_self(), [&](auto& row) {
            row.ram_weight.amount += ram_weight_amount;
        });

        // Update reslimit for sysio.acct for the ram
        update_reslimit("sysio.acct"_n, {0, sys_symbol}, {0, sys_symbol}, sysiosystem::newaccount_ram);

        return new_username;
    }
};

// namespace roa
