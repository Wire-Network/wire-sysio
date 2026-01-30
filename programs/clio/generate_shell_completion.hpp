#pragma once

#include <sstream>
#include <string>

namespace detail {

inline auto escape_single_quote = [](std::string s) {
   size_t pos = 0;
   while ((pos = s.find("'", pos)) != std::string::npos) {
      s.replace(pos, 1, "\\'");
      pos += 2;
   }
   return s;
};

} // namespace detail

// --------------------------------------------------------
inline void generate_bash_completion(const CLI::App& app) {
   // Helper to collect option names
   auto collect_names = [](const CLI::Option* opt, std::vector<std::string>& out) {
      for (auto& s : opt->get_snames())
         out.push_back("-" + s);
      for (auto& l : opt->get_lnames())
         out.push_back("--" + l);
   };

   // Helper to gather subcommands, options, and positionals
   auto gather = [&collect_names](const CLI::App& app_inst, std::vector<std::string>& subcmds,
                                  std::vector<std::string>& options, std::vector<std::string>& positionals) {
      for (auto* sc : app_inst.get_subcommands({})) {
         if (!sc->get_name().empty())
            subcmds.push_back(sc->get_name());
      }
      for (auto* opt : app_inst.get_options({})) {
         if (opt->nonpositional()) {
            collect_names(opt, options);
         } else if (opt->get_positional()) {
            // Positional argument - add its name
            std::string pos_name = opt->get_name();
            if (!pos_name.empty())
               positionals.push_back(pos_name);
         }
      }
   };

   // Join vector of strings
   auto join = [](const std::vector<std::string>& v) {
      std::ostringstream os;
      bool first = true;
      for (auto& s : v) {
         if (!first)
            os << ' ';
         first = false;
         os << s;
      }
      return os.str();
   };

   std::vector<std::string> subs, opts, positionals;
   gather(app, subs, opts, positionals);

   std::cout << "# Bash completion for clio\n"
      << "# Install: Copy to ~/.local/share/bash-completion/completions/clio\n"
      << "_clio_complete() {\n"
      << "    local cur prev words cword\n"
      << "    _init_completion -n : || return\n\n"
      << "    # top-level\n"
      << "    case \"${COMP_WORDS[1]}\" in\n";

   // Generate per-subcommand cases
   for (auto* sc : app.get_subcommands({})) {
      if (sc->get_name().empty())
         continue;
      std::vector<std::string> sc_subs, sc_opts, sc_positionals;
      gather(*sc, sc_subs, sc_opts, sc_positionals);
      std::cout << "        " << sc->get_name() << ")\n";
      std::cout << "            COMPREPLY=( $(compgen -W \""
         << join(sc_subs) << " " << join(sc_opts);
      if (!sc_positionals.empty()) {
         std::cout << " " << join(sc_positionals);
      }
      std::cout << "\" -- \"$cur\") )\n";
      std::cout << "            return ;;\n";
   }

   // Default (root)
   std::cout << "        *)\n";
   std::cout << "            COMPREPLY=( $(compgen -W \"" << join(subs) << " " << join(opts);
   if (!positionals.empty()) {
      std::cout << " " << join(positionals);
   }
   std::cout << "\" -- \"$cur\") )\n";
   std::cout << "            return ;;\n"
      << "    esac\n"
      << "}\n\n"
      << "# Requires bash-completion\n"
      << "if declare -F _init_completion >/dev/null 2>&1; then\n"
      << "  complete -F _clio_complete clio\n"
      << "fi\n";
};

// --------------------------------------------------------
inline void generate_zsh_completion(const CLI::App& app) {
   // Helper to collect option names
   auto collect_names = [](const CLI::Option* opt, std::vector<std::string>& out) {
      for (auto& s : opt->get_snames())
         out.push_back("-" + s);
      for (auto& l : opt->get_lnames())
         out.push_back("--" + l);
   };

   // Helper to gather subcommands, options, and positionals
   auto gather = [&collect_names](const CLI::App& app_inst, std::vector<std::string>& subcmds,
                                  std::vector<std::string>& options, std::vector<std::string>& positionals) {
      for (auto* sc : app_inst.get_subcommands({})) {
         if (!sc->get_name().empty())
            subcmds.push_back(sc->get_name());
      }
      for (auto* opt : app_inst.get_options({})) {
         if (opt->nonpositional()) {
            collect_names(opt, options);
         } else if (opt->get_positional()) {
            std::string pos_name = opt->get_name();
            if (!pos_name.empty())
               positionals.push_back(pos_name);
         }
      }
   };

   std::vector<std::string> subs, opts, positionals;
   gather(app, subs, opts, positionals);

   std::cout << "#compdef clio\n"
      << "# Install: Place in a directory in your $fpath (e.g., ~/.config/zsh/completions/)\n\n"
      << "_clio() {\n"
      << "    local -a commands\n"
      << "    commands=(\n";

   // Add top-level subcommands
   for (auto* sc : app.get_subcommands({})) {
      if (!sc->get_name().empty()) {
         std::string desc = detail::escape_single_quote(sc->get_description());
         std::cout << "        '" << sc->get_name() << ":" << desc << "'\n";
      }
   }

   std::cout << "    )\n\n"
      << "    _arguments -C \\\n";

   // Add top-level options
   for (auto* opt : app.get_options({})) {
      if (opt->nonpositional()) {
         std::string desc = detail::escape_single_quote(opt->get_description());
         for (auto& l : opt->get_lnames()) {
            std::cout << "        '--" << l << "[" << desc << "]' \\\n";
         }
         for (auto& s : opt->get_snames()) {
            std::cout << "        '-" << s << "[" << desc << "]' \\\n";
         }
      }
   }

   // Add top-level positionals
   int pos_idx = 1;
   for (auto& pos : positionals) {
      std::cout << "        '" << pos_idx << ":" << pos << ":' \\\n";
      pos_idx++;
   }

   std::cout << "        '1: :->cmds' \\\n"
      << "        '*::arg:->args'\n\n"
      << "    case $state in\n"
      << "        cmds)\n"
      << "            _describe 'command' commands\n"
      << "            ;;\n"
      << "        args)\n"
      << "            case $words[1] in\n";

   // Generate per-subcommand cases
   for (auto* sc : app.get_subcommands({})) {
      if (sc->get_name().empty())
         continue;
      std::vector<std::string> sc_subs, sc_opts, sc_positionals;
      gather(*sc, sc_subs, sc_opts, sc_positionals);

      std::cout << "                " << sc->get_name() << ")\n";
      if (!sc_subs.empty()) {
         std::cout << "                    local -a subcommands\n"
            << "                    subcommands=(\n";
         for (auto* ssc : sc->get_subcommands({})) {
            if (!ssc->get_name().empty()) {
               std::string desc = detail::escape_single_quote(ssc->get_description());
               std::cout << "                        '" << ssc->get_name() << ":" << desc << "'\n";
            }
         }
         std::cout << "                    )\n"
            << "                    _describe 'subcommand' subcommands\n";
      }
      // Add positionals for this subcommand
      if (!sc_positionals.empty()) {
         std::cout << "                    _arguments \\\n";
         int sc_pos_idx = 2; // Start from 2 since 1 is the subcommand itself
         for (auto& pos : sc_positionals) {
            std::cout << "                        '" << sc_pos_idx << ":" << pos << ":' \\\n";
            sc_pos_idx++;
         }
      }
      std::cout << "                    ;;\n";
   }

   std::cout << "            esac\n"
      << "            ;;\n"
      << "    esac\n"
      << "}\n\n"
      << "_clio\n";
};

// --------------------------------------------------------
inline void generate_fish_completion(const CLI::App& app) {
   // Helper to collect option names
   auto collect_names = [](const CLI::Option* opt, std::vector<std::string>& out) {
      for (auto& s : opt->get_snames())
         out.push_back("-" + s);
      for (auto& l : opt->get_lnames())
         out.push_back("--" + l);
   };

   // Helper to gather subcommands, options, and positionals
   auto gather = [&collect_names](const CLI::App& app_inst, std::vector<std::string>& subcmds,
                                  std::vector<std::string>& options, std::vector<std::string>& positionals) {
      for (auto* sc : app_inst.get_subcommands({})) {
         if (!sc->get_name().empty())
            subcmds.push_back(sc->get_name());
      }
      for (auto* opt : app_inst.get_options({})) {
         if (opt->nonpositional()) {
            collect_names(opt, options);
         } else if (opt->get_positional()) {
            std::string pos_name = opt->get_name();
            if (!pos_name.empty())
               positionals.push_back(pos_name);
         }
      }
   };

   std::vector<std::string> subs, opts, positionals;
   gather(app, subs, opts, positionals);

   std::cout << "# Fish completions for clio\n"
      << "# Install: Copy to ~/.config/fish/completions/clio.fish\n";

   // Top-level subcommands
   for (auto& s : subs) {
      std::cout << "complete -c clio -f -n 'not __fish_seen_subcommand_from "
         << s << "' -a '" << s << "' -d 'subcommand'\n";
   }

   // Root options
   for (auto* opt : app.get_options({})) {
      if (opt->nonpositional()) {
         std::string desc = detail::escape_single_quote(opt->get_description());
         for (auto& l : opt->get_lnames()) {
            std::cout << "complete -c clio -f -l " << l;
            if (!desc.empty())
               std::cout << " -d '" << desc << "'";
            std::cout << "\n";
         }
         for (auto& s : opt->get_snames()) {
            std::cout << "complete -c clio -f -s " << s;
            if (!desc.empty())
               std::cout << " -d '" << desc << "'";
            std::cout << "\n";
         }
      }
   }

   // Root positionals
   for (auto& pos : positionals) {
      std::cout << "complete -c clio -f -n 'not __fish_seen_subcommand_from";
      for (auto& s : subs) {
         std::cout << " " << s;
      }
      std::cout << "' -a '(" << pos << ")'\n";
   }

   // Per-subcommand options and positionals
   for (auto* sc : app.get_subcommands({})) {
      if (sc->get_name().empty())
         continue;
      std::vector<std::string> sc_subs, sc_opts, sc_positionals;
      gather(*sc, sc_subs, sc_opts, sc_positionals);

      for (auto& ssc : sc_subs) {
         std::cout << "complete -c clio -n '__fish_seen_subcommand_from " << sc->get_name() << "'"
            << " -a '" << ssc << "' -d 'subcommand'\n";
      }

      for (auto* opt : sc->get_options({})) {
         if (opt->nonpositional()) {
            std::string desc = detail::escape_single_quote(opt->get_description());
            for (auto& l : opt->get_lnames()) {
               std::cout << "complete -c clio -n '__fish_seen_subcommand_from " << sc->get_name() << "'"
                  << " -f -l " << l;
               if (!desc.empty())
                  std::cout << " -d '" << desc << "'";
               std::cout << "\n";
            }
            for (auto& s : opt->get_snames()) {
               std::cout << "complete -c clio -n '__fish_seen_subcommand_from " << sc->get_name() << "'"
                  << " -f -s " << s;
               if (!desc.empty())
                  std::cout << " -d '" << desc << "'";
               std::cout << "\n";
            }
         }
      }

      // Positionals for this subcommand
      for (auto& pos : sc_positionals) {
         std::cout << "complete -c clio -n '__fish_seen_subcommand_from " << sc->get_name() << "'"
            << " -f -a '(" << pos << ")'\n";
      }
   }
};