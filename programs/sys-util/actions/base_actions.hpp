#pragma once

#include <CLI/CLI.hpp>
#include <memory>

class sys_util_exception_handler {
public:
   sys_util_exception_handler() {}
   ~sys_util_exception_handler() {}
   void print_exception() noexcept;
};

template<class action_options, class exception_handler = sys_util_exception_handler>
class base_actions {
protected:
   std::shared_ptr<action_options> opt;
   std::unique_ptr<exception_handler> exh;

   base_actions() : opt(std::make_shared<action_options>()), exh(std::make_unique<exception_handler>()) {}
   void print_exception() noexcept { exh->print_exception(); };

public:
   virtual ~base_actions() {}
   virtual void setup(CLI::App& app) = 0;
};