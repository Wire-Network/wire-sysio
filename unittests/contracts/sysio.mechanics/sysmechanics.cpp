#include <sysiolib/contracts/sysio/sysio.hpp>
//#include <sysiolib/print.hpp>
#include <math.h>
#pragma precision=log10l(ULLONG_MAX)/2
typedef enum { FALSE=0, TRUE=1 } BOOL;

// Max when calculating primes in cpu test
#define CPU_PRIME_MAX 375

// Number of rows to write/read in ram test
#define RAM_ROWS 75

using namespace sysio;

CONTRACT sysmechanics : public sysio::contract {
    public:
        using contract::contract;

        /**
         * Simple CPU benchmark that calculates Mersenne prime numbers.
         */
        [[sysio::action]] void cpu() {
            // Only let us run this
            require_auth(_self);
            
            int p;

            //sysio::print_f("Mersenne primes:\n");
            for (p = 2; p <= CPU_PRIME_MAX; p += 1) {
                if (is_prime(p) && is_mersenne_prime(p)) {
                    // We need to keep an eye on this to make sure it doesn't get optimized out. So far so good.
                    //sysio::print_f(" %u", p);
                }
            }
        }

        /**
         * Simple SYS RAM benchmark which reads and writes a table.
         */
        [[sysio::action]] void ram() {
            ramdata_index ramdata(_self, _self.value);

            // Only let us run this
            require_auth(_self);

            int i;

            // Write
            for (i = 0; i < RAM_ROWS; i++) {
                ramdata.emplace(_self, [&](auto& row) {
                    row.id = i;
                    row.one = "aloha";
                });
            }

            // Read
            for (const auto& row: ramdata) {
                //sysio::print_f("read %d: %s\n", row.id, row.one);
                i = row.id;
            }

            // Delete
            for(auto itr = ramdata.begin(); itr != ramdata.end();) {
                itr = ramdata.erase(itr);
            }
        }

        /**
         * Simple SYS Net benchmark which just accepts any string passed in.
         */
        [[sysio::action]] void net(std::string input) {
            // Only let us run this
            require_auth(_self);
        }

    private:

        BOOL is_prime(int p) {
            if (p == 2) {
                return TRUE;
            } else if (p <= 1 || p % 2 == 0) {
                return FALSE;
            }

            BOOL prime = TRUE;
            const int to = sqrt(p);
            int i;
            for (i = 3; i <= to; i += 2) {  
                if (!((prime = BOOL(p)) % i)) break;
            }
            return prime;
        }
      
        BOOL is_mersenne_prime(int p) {
            if (p == 2) return TRUE;

            const long long unsigned m_p = (1LLU << p) - 1;
            long long unsigned s = 4;
            int i;
            for (i = 3; i <= p; i++) {
                s = (s * s - 2) % m_p;
            }
            return BOOL(s == 0);
        }

        // @abi table ramdata i64
        struct [[sysio::table]] ramdata {
            uint64_t id;
            std::string one;

            uint64_t primary_key() const { return id; }

            SYSLIB_SERIALIZE(ramdata, (id)(one))
        };

        typedef sysio::multi_index<"ramdata"_n, ramdata> ramdata_index;

};

SYSIO_DISPATCH(sysmechanics, (cpu)(ram)(net))
