#include "base_actions.hpp"

struct sc_generic_options {
};

class generic_actions : public base_actions<sc_generic_options> {
public:
   generic_actions() : base_actions() {}
   void setup(CLI::App& app);

   // callbacks
   void cb_version(bool full);
};