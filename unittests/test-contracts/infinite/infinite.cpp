#include "infinite.hpp"

using namespace sysio;

constexpr size_t cpu_prime_max = 1024u;

bool is_prime(int p) {
   if (p == 2) {
      return true;
   } else if (p <= 1 || p % 2 == 0) {
      return false;
   }

   bool prime = true;
   const int to = sqrt(p);
   for (int i = 3; i <= to; i += 2) {
      if (p % i == 0) {
         prime = false;
         break;
      }
   }
   return prime;
}

bool is_mersenne_prime(int p) {
   if (p == 2) return true;

   long long unsigned s = 4;
   const long long unsigned m_p = (1LLU << (p % (sizeof(s) * 8))) - 1;
   int i;
   for (i = 3; i <= p; i++) {
      s = (s * s - 2) % m_p;
   }
   return bool(s == 0);
}


void infinite::runslow() {
   print("Im a slow action");

   volatile size_t cpu_work_sink = 0;
   for (size_t p = 2; p <= cpu_prime_max; p += 1) {
      if (is_prime(p) && is_mersenne_prime(p)) {
         cpu_work_sink += p;
      }
   }
}

void infinite::runforever() {
   print("Im a forever action");
   constexpr size_t max_cpu_prime = std::numeric_limits<size_t>::max();

   while (true) {
      volatile size_t cpu_work_sink = 0;
      for (size_t p = 2; p <= max_cpu_prime; p += 1) {
         if (is_prime(p) && is_mersenne_prime(p)) {
            cpu_work_sink += p;
         }
      }
   }
}

void infinite::segv() {
   volatile int* p = nullptr;
   volatile int x = *p; // cause segfault
   while (true) {
      x = *++p; // avoid being optimized out
   }
}
