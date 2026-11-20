#include "sysio_test_generator.hpp"

#include "picojson.hpp"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace std;

namespace {
/// wast2json command types for results that must be a NaN of the given class rather than an exact bit pattern.
constexpr string_view command_assert_return_canonical_nan = "assert_return_canonical_nan";
constexpr string_view command_assert_return_arithmetic_nan = "assert_return_arithmetic_nan";

/// A function emitted into the generated contract that checks a float result by its bit pattern.
struct check_helper {
   string_view name;
   string_view definition;
};

// Each float check is a call to one of these, so a sub_apply holds only the call and its arguments; inlining the
// comparison at -O0 gives every check its own temporaries and exceeds the per-function local limit.
// A canonical NaN masks off only the sign bit, so the payload must be exactly the quiet bit; an arithmetic NaN also
// masks off the low payload bits, so only the exponent and the quiet bit must be set.
constexpr check_helper f32_bits_check = {"check_f32_bits",
                                         R"(   void check_f32_bits(float actual, uint32_t expected, const char* msg) {
      sysio::check(__builtin_bit_cast(uint32_t, actual) == expected, msg);
   }

)"};
constexpr check_helper f32_canonical_nan_check = {"check_f32_canonical_nan",
                                                  R"(   void check_f32_canonical_nan(float actual, const char* msg) {
      sysio::check((__builtin_bit_cast(uint32_t, actual) & 0x7fffffff) == 0x7fc00000, msg);
   }

)"};
constexpr check_helper f32_arithmetic_nan_check = {"check_f32_arithmetic_nan",
                                                   R"(   void check_f32_arithmetic_nan(float actual, const char* msg) {
      sysio::check((__builtin_bit_cast(uint32_t, actual) & 0x7fc00000) == 0x7fc00000, msg);
   }

)"};
constexpr check_helper f64_bits_check = {"check_f64_bits",
                                         R"(   void check_f64_bits(double actual, uint64_t expected, const char* msg) {
      sysio::check(__builtin_bit_cast(uint64_t, actual) == expected, msg);
   }

)"};
constexpr check_helper f64_canonical_nan_check = {"check_f64_canonical_nan",
                                                  R"(   void check_f64_canonical_nan(double actual, const char* msg) {
      sysio::check((__builtin_bit_cast(uint64_t, actual) & 0x7fffffffffffffff) == 0x7ff8000000000000, msg);
   }

)"};
constexpr check_helper f64_arithmetic_nan_check = {"check_f64_arithmetic_nan",
                                                   R"(   void check_f64_arithmetic_nan(double actual, const char* msg) {
      sysio::check((__builtin_bit_cast(uint64_t, actual) & 0x7ff8000000000000) == 0x7ff8000000000000, msg);
   }

)"};

/// Every check helper, in the order a generated contract defines the ones it uses.
constexpr check_helper check_helpers[] = {f32_bits_check, f32_canonical_nan_check, f32_arithmetic_nan_check,
                                          f64_bits_check, f64_canonical_nan_check, f64_arithmetic_nan_check};

/// f32 exponent and fraction masks, to recognise NaN arguments by their bit pattern.
constexpr uint64_t f32_exponent_mask = 0x7f800000;
constexpr uint64_t f32_fraction_mask = 0x007fffff;
/// f64 exponent and fraction masks, to recognise NaN arguments by their bit pattern.
constexpr uint64_t f64_exponent_mask = 0x7ff0000000000000;
constexpr uint64_t f64_fraction_mask = 0x000fffffffffffff;

/// Spec exports whose sys-vm implementation returns a NaN operand unchanged; see get_pinned_nan_result.
const set<string> nan_passthrough_functions = {"min", "max", "floor", "ceil", "trunc", "nearest"};
} // namespace

const set<string> blacklist_memory_clearing = { "address.0",      "address.2",      "address.3",      "address.4",
                                                "float_exprs.59", "float_exprs.60", "float_memory.0", "float_memory.1",
                                                "float_memory.2", "float_memory.3", "float_memory.4", "float_memory.5",
                                                "memory.25",      "memory_trap.1",  "start.3",        "start.4" };

const set<string> whitelist_force_check_throw = { "memory.6", "memory.7" };

const string include_sysio = "#include <sysio/sysio.hpp>\n\n";
const string extern_c      = "extern \"C\" {\n";
const string apply_func    = "   void apply(uint64_t, uint64_t, uint64_t test_to_run) {\n";
const string mem_clear     = "      volatile uint64_t* r = (uint64_t*)0;\n      *r = 0;\n";

// NOTE: Changing this will likely break one or more tests.
const int NUM_TESTS_PER_SUB_APPLY = 100;

struct spec_test_function_state {
   set<string>      already_written_funcs;
   map<string, int> name_to_index;
   int              index = 0;
   /// Names of the check helpers the sub_apply functions call.
   set<string_view> used_check_helpers;
};

/**
 * Write the C++ source of one spec test module's contract.
 *
 * @param file               output stream
 * @param test_name          module name, e.g. `f32.0`
 * @param funcs              stub definitions of the test module's exports, in map order
 * @param used_check_helpers check helpers the sub_apply functions call; only these are defined
 * @param sub_applies        sub_apply definitions
 * @param apply              body of the switch that dispatches to the sub_apply functions
 */
void write_file(ofstream& file, string test_name, string funcs, const set<string_view>& used_check_helpers,
                string sub_applies, string apply) {
   stringstream out;
   string       end_brace = "}";

   out << include_sysio;
   out << extern_c;
   out << funcs;
   // After the stubs: the merge locates the stubs right after the imports.
   for (const check_helper& helper : check_helpers) {
      if (used_check_helpers.count(helper.name)) {
         out << helper.definition;
      }
   }
   out << sub_applies;
   out << apply_func;
   if (!blacklist_memory_clearing.count(test_name)) {
      out << mem_clear;
   }
   out << apply;
   out << "   }\n";
   out << "}\n";

   file << out.str();
   out.str("");
}

void write_map_file(ofstream& file, spec_test_function_state& func_state) {
   stringstream out;

   out << "{\n";
   bool first = true;
   for (const auto& [name, index] : func_state.name_to_index) {
      if (first) {
         first = false;
      } else {
         out << ",\n";
      }
      out << "  \"" << name << "\""
          << " : " << index;
   }
   out << "\n}\n";

   file << out.str();
   out.str("");
}

string c_type(string wasm_type) {
   string c_type = "";
   if (wasm_type == "i32") {
      c_type = "int32_t";
   } else if (wasm_type == "i64") {
      c_type = "int64_t";
   } else if (wasm_type == "f32") {
      c_type = "float";
   } else if (wasm_type == "f64") {
      c_type = "double";
   } else {
      c_type = "void";
   }

   return c_type;
}

/**
 * Unsigned integer type holding the IEEE-754 bit pattern of a wasm float type.
 *
 * wast2json encodes f32/f64 argument and expected values as the decimal bit pattern.
 *
 * @return "uint32_t" for f32, "uint64_t" for f64, and an empty string for every other type.
 */
string float_bits_type(const string& wasm_type) {
   if (wasm_type == "f32") {
      return "uint32_t";
   }
   if (wasm_type == "f64") {
      return "uint64_t";
   }
   return "";
}

/**
 * Check helper for a float result.
 *
 * @param wasm_type      the type of the result
 * @param canonical_nan  the result must be a canonical NaN
 * @param arithmetic_nan the result must be an arithmetic NaN
 * @return the NaN class check when a NaN is required, the exact bit pattern check otherwise, and nullptr for a
 *         non-float result. Exits with an error if a NaN is required of a non-float result.
 */
const check_helper* get_float_check_helper(const string& wasm_type, bool canonical_nan, bool arithmetic_nan) {
   if (wasm_type == "f32") {
      return canonical_nan ? &f32_canonical_nan_check : arithmetic_nan ? &f32_arithmetic_nan_check : &f32_bits_check;
   }
   if (wasm_type == "f64") {
      return canonical_nan ? &f64_canonical_nan_check : arithmetic_nan ? &f64_arithmetic_nan_check : &f64_bits_check;
   }
   if (canonical_nan || arithmetic_nan) {
      cerr << "NaN assert expects a float result, got '" << wasm_type << "'\n";
      exit(EXIT_FAILURE);
   }
   return nullptr;
}

/// Whether a wast2json f32/f64 value (a decimal bit pattern) is a NaN.
bool is_nan_value(const string& wasm_type, const string& value) {
   if (wasm_type != "f32" && wasm_type != "f64") {
      return false;
   }
   const uint64_t bits = stoull(value);
   const uint64_t exponent_mask = wasm_type == "f32" ? f32_exponent_mask : f64_exponent_mask;
   const uint64_t fraction_mask = wasm_type == "f32" ? f32_fraction_mask : f64_fraction_mask;
   return (bits & exponent_mask) == exponent_mask && (bits & fraction_mask) != 0;
}

/**
 * Result pinned for an assert_return_arithmetic_nan command on a function that passes NaN operands through.
 *
 * sys-vm's min, max, floor, ceil, trunc and nearest (sys-vm softfloat.hpp for the interpreter and JIT, chain
 * softfloat.cpp for OC) return the first NaN operand unchanged, so a signaling NaN stays signaling where the spec
 * requires an arithmetic NaN. The check pins that result, so a change to either implementation fails it.
 *
 * @param command_type the command's `type`
 * @param field        the invoked export
 * @param params       the arguments, as (type, value) pairs
 * @param return_type  the type of the result
 * @return the first NaN argument's bit pattern, or nullopt for any other command. Exits with an error if a pinned
 *         command has no NaN argument or its first NaN argument differs in type from the result.
 */
optional<string> get_pinned_nan_result(const string& command_type, const string& field,
                                       const vector<pair<string, string>>& params, const string& return_type) {
   if (command_type != command_assert_return_arithmetic_nan || !nan_passthrough_functions.count(field)) {
      return nullopt;
   }
   for (const auto& [type, value] : params) {
      if (is_nan_value(type, value)) {
         if (type != return_type) {
            cerr << command_type << " on " << field << " returns " << return_type << " but its NaN argument is " << type
                 << "\n";
            exit(EXIT_FAILURE);
         }
         return value;
      }
   }
   cerr << command_type << " on " << field << " has no NaN argument\n";
   exit(EXIT_FAILURE);
}

vector<pair<string, string>> get_params(picojson::object action) {
   vector<pair<string, string>> params = {};

   auto args = action["args"].get<picojson::array>();
   for (auto a : args) {
      auto arg = a.get<picojson::object>();

      params.emplace_back(arg["type"].to_str(), arg["value"].to_str());
   }

   return params;
}

pair<string, string> get_expected_return(picojson::object test) {
   pair<string, string> expected_return;

   auto expecteds = test["expected"].get<picojson::array>();
   for (auto e : expecteds) {
      auto   expect   = e.get<picojson::object>();
      string type     = expect["type"].to_str();
      string value    = expect["value"].to_str();
      expected_return = make_pair(type, value);
   }

   return expected_return;
}

string write_test_function(string function_name, picojson::object test, spec_test_function_state& func_state) {
   stringstream out;

   auto action = test["action"].get<picojson::object>();

   vector<pair<string, string>> params          = get_params(action);
   pair<string, string>         expected_return = get_expected_return(test);

   if (!func_state.already_written_funcs.insert(function_name).second) {
      return "";
   }

   func_state.name_to_index[function_name] = func_state.index;
   ++func_state.index;

   auto [return_type, return_val] = expected_return;
   out << "   " << c_type(return_type) << " ";
   out << function_name;
   out << "(";

   if (params.size() > 0) {
      for (auto p = params.begin(); p != params.end() - 1; p++) {
         auto [type, _] = *p;
         out << c_type(type) << ", ";
      }

      auto [type, _] = *(params.end() - 1);
      out << c_type(type);
   }

   out << ") {\n";

   if (return_type == "i32" || return_type == "i64") {
      out << "   "
          << "   return 0;";
   } else if (return_type == "f32" || return_type == "f64") {
      out << "   "
          << "   return 0.0f;";
   } else {
      out << "   "
          << "   return;";
   }

   out << "\n   }\n\n";
   return out.str();
}

/**
 * Write the call of one spec test command and the check of its result.
 *
 * @param function_name      stub name of the invoked export
 * @param test               the wast2json command
 * @param var_index          index that makes the generated local names and the failure message unique
 * @param used_check_helpers receives the check helper the call uses, if any
 * @return the generated statements
 */
string write_test_function_call(string function_name, picojson::object test, int var_index,
                                set<string_view>& used_check_helpers) {
   stringstream out;
   stringstream func_call;

   auto                         action          = test["action"].get<picojson::object>();
   vector<pair<string, string>> params          = get_params(action);
   pair<string, string>         expected_return = get_expected_return(test);

   auto [return_type, return_val] = expected_return;
   const string command_type = test["type"].to_str();
   const optional<string> pinned = get_pinned_nan_result(command_type, action["field"].to_str(), params, return_type);
   if (pinned) {
      return_val = *pinned;
   }

   int param_index_offset = var_index;
   if (params.size() > 0) {
      int param_index = 0;
      for (auto p = params.begin(); p != params.end(); p++) {
         auto [type, value] = *p;
         if (type == "f32") {
            out << "      "
                << "int32_t "
                << "y" << param_index << param_index_offset << " = " << value << ";\n";
            param_index++;
         } else if (type == "f64") {
            out << "      "
                << "int64_t "
                << "y" << param_index << param_index_offset << " = " << value << ";\n";
            param_index++;
         }
      }
   }

   const bool has_expected_value = return_val != "" && return_val != "null";
   const bool canonical_nan = !pinned && command_type == command_assert_return_canonical_nan;
   const bool arithmetic_nan = !pinned && command_type == command_assert_return_arithmetic_nan;
   const bool is_nan_command = canonical_nan || arithmetic_nan;
   // Float results are checked by bit pattern. Comparing as float would convert both sides to float and round away
   // low-order bits, so a wrong result could still pass.
   const check_helper* float_check = has_expected_value || is_nan_command
                                        ? get_float_check_helper(return_type, canonical_nan, arithmetic_nan)
                                        : nullptr;
   const bool needs_local_return = float_check == nullptr && has_expected_value && c_type(return_type) != "void";

   if (needs_local_return) {
      func_call << "   "
                << "   " << c_type(return_type) << " "
                << "x" << var_index << " = ";
   }

   func_call << function_name << "(";

   if (params.size() > 0) {
      bool first       = true;
      int  param_index = 0;
      for (const auto& [type, value] : params) {
         if (first) {
            first = false;
         } else {
            func_call << ",";
         }

         if (type == "f32") {
            func_call << "*(float*)&"
                      << "y" << param_index << param_index_offset;
            param_index++;
         } else if (type == "f64") {
            func_call << "*(double*)&"
                      << "y" << param_index << param_index_offset;
            param_index++;
         } else {
            func_call << "(" << c_type(type) << ") " << value;
         }
      }
   }
   func_call << ")";

   const string fail_message = "\"" + function_name + " fail " + to_string(var_index) + "\"";
   if (float_check) {
      used_check_helpers.insert(float_check->name);
      out << "      " << float_check->name << "(" << func_call.str();
      if (!is_nan_command) {
         out << ", (" << float_bits_type(return_type) << ")" << return_val;
      }
      out << ", " << fail_message << ");\n\n";
      return out.str();
   }

   func_call << ";";
   if (needs_local_return) {
      out << func_call.str() << "\n";
   }

   if (has_expected_value) {
      out << "   "
          << "   "
          << "sysio::check(";
      if (needs_local_return) {
         out << "x" << var_index;
      } else {
         out << func_call.str();
      }
      out << " == ";
      out << "(" << c_type(return_type) << ")" << return_val;
      out << ", " << fail_message;
      out << ");\n\n";
   } else {
      // If there's no expected return, just call the function to prove it doesn't blow up.
      out << "   "
          << "   " << func_call.str() << "\n\n";
   }
   return out.str();
}

void usage(const char* name) {
   std::cerr << "Usage:\n"
             << "  " << name << " [json file created by wast2json]\n";
   std::exit(2);
}

map<string, vector<picojson::object>> get_file_func_mappings(picojson::value v) {
   map<string, vector<picojson::object>> file_func_mappings;
   auto&                                 o        = v.get<picojson::object>();
   string                                filename = "";

   picojson::array commands = o["commands"].get<picojson::array>();

   for (auto i = o.begin(); i != o.end(); i++) {
      if (i->first == "commands") {
         for (const auto& o : i->second.get<picojson::array>()) {
            auto obj = o.get<picojson::object>();
            if (obj["type"].to_str() == "module") {
               filename                     = obj["filename"].to_str();
               file_func_mappings[filename] = {};
            }
            if (obj["type"].to_str() == "assert_return" || obj["type"].to_str() == "action" ||
                obj["type"].to_str() == "assert_exhaustion" ||
                obj["type"].to_str() == command_assert_return_canonical_nan ||
                obj["type"].to_str() == command_assert_return_arithmetic_nan || obj["type"].to_str() == "assert_trap") {
               file_func_mappings[filename].push_back(obj);
            }
         }
      }
   }

   return file_func_mappings;
}

string create_sub_apply(string func_calls, string name) {
   stringstream ss;
   ss << "   void " << name << "() {\n";
   ss << func_calls;
   ss << "   }\n";
   return ss.str();
}

int main(int argc, char** argv) {
   ifstream     ifs;
   stringstream ss;
   if (argc != 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
      usage(argc ? argv[0] : "sysio_test_generator");
   }
   ifs.open(argv[1]);
   if (!ifs) {
      std::cerr << "Cannot open file: " << argv[1] << std::endl;
      return EXIT_FAILURE;
   }
   string s;
   while (getline(ifs, s)) { ss << s; }
   ifs.close();

   picojson::value v;
   picojson::parse(v, ss.str());
   string test_suite_name;

   map<string, vector<picojson::object>> file_func_mappings = get_file_func_mappings(v);
   map<string, map<int, string>>         test_mappings;

   vector<spec_test> spec_tests;

   for (const auto& f : file_func_mappings) {
      ofstream ofs_cpp;
      ofstream ofs_map;

      stringstream test_funcs;
      stringstream sub_apply_funcs;
      stringstream apply_func;

      vector<string> sub_applies;

      int    pos          = f.first.find_last_of('.');
      string test_name    = f.first.substr(0, pos);
      string out_file_cpp = test_name + ".wasm.cpp";
      string out_file_map = test_name + ".wasm.map";

      ofs_cpp.open(out_file_cpp, ofstream::out);
      ofs_map.open(out_file_map, ofstream::out);

      vector<picojson::object> assert_trap_tests;
      vector<picojson::object> assert_return_tests;

      spec_test_function_state func_state;

      for (auto test : f.second) {
         string type_test     = test["type"].to_str();
         auto   action        = test["action"].get<picojson::object>();
         string function_name = action["field"].to_str();
         function_name        = "_" + convert_to_valid_cpp_identifier(function_name);

         test_funcs << write_test_function(function_name, test, func_state);
         int is_whitelisted = whitelist_force_check_throw.count(test_name);
         if (type_test == "assert_trap" || type_test == "assert_exhaustion" || is_whitelisted) {
            assert_trap_tests.push_back(test);
         } else {
            assert_return_tests.push_back(test);
         }
      }

      int sub_apply_index = 0;
      for (auto test : assert_trap_tests) {
         auto   action        = test["action"].get<picojson::object>();
         string function_name = action["field"].to_str();
         function_name        = "_" + convert_to_valid_cpp_identifier(function_name);

         string name = "sub_apply_" + to_string(sub_apply_index);
         sub_applies.push_back(name);
         string func_call =
            write_test_function_call(function_name, test, sub_apply_index, func_state.used_check_helpers);
         string sub_apply = create_sub_apply(func_call, name);

         sub_apply_funcs << sub_apply;
         test_mappings[test_name].insert(std::make_pair(sub_apply_index, "assert_trap"));
         ++sub_apply_index;
      }

      int assert_trap_end_index = sub_apply_index;

      stringstream ss;
      int          i            = 1;
      int          length_tests = assert_return_tests.size();
      for (auto test : assert_return_tests) {
         auto   action        = test["action"].get<picojson::object>();
         string function_name = action["field"].to_str();
         function_name        = "_" + convert_to_valid_cpp_identifier(function_name);

         if (i % NUM_TESTS_PER_SUB_APPLY == 0 || i == length_tests) {
            string name = "sub_apply_" + to_string(sub_apply_index);
            sub_applies.push_back(name);

            ss << write_test_function_call(function_name, test, i, func_state.used_check_helpers);
            sub_apply_funcs << create_sub_apply(ss.str(), name);
            ss.str("");
            test_mappings[test_name].insert(std::make_pair(sub_apply_index, ""));
            ++sub_apply_index;
         } else {
            ss << write_test_function_call(function_name, test, i, func_state.used_check_helpers);
         }
         ++i;
      }

      spec_tests.push_back(spec_test{ test_name, 0, assert_trap_end_index, assert_trap_end_index, sub_apply_index });

      int index = 0;
      apply_func << "      switch(test_to_run) {\n";
      for (auto sub_apply : sub_applies) {
         apply_func << "         case " << index << ":\n";
         apply_func << "            " << sub_apply << "();"
                    << "\n";
         apply_func << "            break;\n";
         ++index;
      }
      apply_func << "      }\n";

      write_file(ofs_cpp, test_name, test_funcs.str(), func_state.used_check_helpers, sub_apply_funcs.str(),
                 apply_func.str());
      write_map_file(ofs_map, func_state);
   }

   write_tests(spec_tests);
}
