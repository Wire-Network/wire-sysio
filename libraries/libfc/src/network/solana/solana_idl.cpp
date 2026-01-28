// SPDX-License-Identifier: MIT
#include <fc/network/solana/solana_idl.hpp>
#include <fc/network/solana/solana_borsh.hpp>

#include <fstream>
#include <sstream>

#include <fc/io/json.hpp>

namespace fc::network::solana::idl {

//=============================================================================
// primitive_type string conversion
//=============================================================================

std::optional<primitive_type> primitive_type_from_string(std::string_view s) {
   // Handle special cases where IDL string differs from enum name
   if (s == "bool")
      return primitive_type::bool_t;
   if (s == "publicKey")
      return primitive_type::pubkey;

   // Try direct magic_enum lookup (works for u8, u16, i32, string, bytes, pubkey, etc.)
   auto result = magic_enum::enum_cast<primitive_type>(s);
   if (result.has_value())
      return result;

   return std::nullopt;
}

std::string_view primitive_type_to_string(primitive_type t) {
   // Handle special cases where we want different output than the enum name
   if (t == primitive_type::bool_t)
      return "bool";
   return magic_enum::enum_name(t);
}

//=============================================================================
// idl_type implementation
//=============================================================================

idl_type idl_type::make_primitive(primitive_type p) {
   idl_type result;
   result.kind = type_kind::primitive;
   result.primitive = p;
   return result;
}

idl_type idl_type::make_defined(std::string name) {
   idl_type result;
   result.kind = type_kind::defined;
   result.defined_name = std::move(name);
   return result;
}

idl_type idl_type::make_option(idl_type inner) {
   idl_type result;
   result.kind = type_kind::option;
   result.option_inner = std::make_shared<idl_type>(std::move(inner));
   return result;
}

idl_type idl_type::make_vec(idl_type element) {
   idl_type result;
   result.kind = type_kind::vec;
   result.vec_element = std::make_shared<idl_type>(std::move(element));
   return result;
}

idl_type idl_type::make_array(idl_type element, size_t len) {
   idl_type result;
   result.kind = type_kind::array;
   result.array_element = std::make_shared<idl_type>(std::move(element));
   result.array_len = len;
   return result;
}

idl_type idl_type::make_tuple(std::vector<idl_type> elements) {
   idl_type result;
   result.kind = type_kind::tuple;
   result.tuple_elements = std::move(elements);
   return result;
}

primitive_type idl_type::get_primitive() const {
   FC_ASSERT(is_primitive() && primitive.has_value(), "Type is not a primitive");
   return *primitive;
}

const std::string& idl_type::get_defined_name() const {
   FC_ASSERT(is_defined() && defined_name.has_value(), "Type is not a defined type");
   return *defined_name;
}

std::string idl_type::to_string() const {
   if (kind == type_kind::primitive)
      return std::string(primitive_type_to_string(*primitive));
   if (kind == type_kind::defined)
      return *defined_name;
   if (kind == type_kind::option)
      return "Option<" + option_inner->to_string() + ">";
   if (kind == type_kind::vec)
      return "Vec<" + vec_element->to_string() + ">";
   if (kind == type_kind::array)
      return "[" + array_element->to_string() + "; " + std::to_string(*array_len) + "]";
   if (kind == type_kind::tuple) {
      std::string result = "(";
      bool first = true;
      for (const auto& elem : *tuple_elements) {
         if (!first)
            result += ", ";
         result += elem.to_string();
         first = false;
      }
      result += ")";
      return result;
   }

   return std::string(magic_enum::enum_name(kind));
}

idl_type idl_type::from_variant(const fc::variant& v) {
   if (v.is_string()) {
      // Simple type like "u64", "string", "pubkey"
      std::string type_str = v.as_string();
      auto prim = primitive_type_from_string(type_str);
      if (prim.has_value()) {
         return make_primitive(*prim);
      }
      // If not a known primitive, treat as defined type
      return make_defined(type_str);
   }

   if (v.is_object()) {
      auto obj = v.get_object();

      // Check for defined type
      if (obj.contains("defined")) {
         return make_defined(obj["defined"].as_string());
      }
      // Check for Option<T>
      if (obj.contains("option")) {
         return make_option(from_variant(obj["option"]));
      }
      // Check for Vec<T>
      if (obj.contains("vec")) {
         return make_vec(from_variant(obj["vec"]));
      }
      // Check for array [T; N]
      if (obj.contains("array")) {
         auto arr = obj["array"].get_array();
         FC_ASSERT(arr.size() == 2, "Array type must have element type and length");
         return make_array(from_variant(arr[0]), arr[1].as_uint64());
      }
      // Check for tuple
      if (obj.contains("tuple")) {
         auto tuple_arr = obj["tuple"].get_array();
         std::vector<idl_type> types;
         types.reserve(tuple_arr.size());
         for (const auto& t : tuple_arr) {
            types.push_back(from_variant(t));
         }
         return make_tuple(std::move(types));
      }
   }

   // Default: return empty primitive (will need to be handled)
   return idl_type{};
}

//=============================================================================
// instruction implementation
//=============================================================================

void instruction::compute_discriminator() { discriminator = compute_instruction_discriminator(name); }

//=============================================================================
// account implementation
//=============================================================================

void account::compute_discriminator() { discriminator = compute_account_discriminator(name); }

//=============================================================================
// program implementation
//=============================================================================

const instruction* program::find_instruction(const std::string& name) const {
   for (const auto& instr : instructions) {
      if (instr.name == name)
         return &instr;
   }
   return nullptr;
}

const account* program::find_account(const std::string& name) const {
   for (const auto& acct : accounts) {
      if (acct.name == name)
         return &acct;
   }
   return nullptr;
}

const type_def* program::find_type(const std::string& name) const {
   for (const auto& t : types) {
      if (t.name == name)
         return &t;
   }
   return nullptr;
}

const error_def* program::find_error(uint32_t code) const {
   for (const auto& e : errors) {
      if (e.code == code)
         return &e;
   }
   return nullptr;
}

//=============================================================================
// Parsing functions
//=============================================================================

namespace {

instruction_arg parse_instruction_arg(const fc::variant& v) {
   instruction_arg arg;
   auto obj = v.get_object();
   arg.name = obj["name"].as_string();
   arg.type = idl_type::from_variant(obj["type"]);
   return arg;
}

instruction_account parse_instruction_account(const fc::variant& v) {
   instruction_account acct;
   auto obj = v.get_object();

   acct.name = obj["name"].as_string();

   // Handle both old format (isMut/isSigner) and new Anchor 0.30+ format (writable/signer)
   if (obj.contains("isMut"))
      acct.is_mut = obj["isMut"].as_bool();
   else if (obj.contains("writable"))
      acct.is_mut = obj["writable"].as_bool();

   if (obj.contains("isSigner"))
      acct.is_signer = obj["isSigner"].as_bool();
   else if (obj.contains("signer"))
      acct.is_signer = obj["signer"].as_bool();

   if (obj.contains("isOptional"))
      acct.is_optional = obj["isOptional"].as_bool();
   else if (obj.contains("optional"))
      acct.is_optional = obj["optional"].as_bool();
   if (obj.contains("docs") && !obj["docs"].is_null()) {
      auto docs_arr = obj["docs"].get_array();
      if (!docs_arr.empty()) {
         std::string docs;
         for (const auto& d : docs_arr) {
            if (!docs.empty())
               docs += "\n";
            docs += d.as_string();
         }
         acct.docs = docs;
      }
   }
   if (obj.contains("address") && !obj["address"].is_null()) {
      acct.address = pubkey::from_base58(obj["address"].as_string());
   }
   if (obj.contains("pda") && !obj["pda"].is_null()) {
      auto pda = obj["pda"].get_object();
      if (pda.contains("seeds")) {
         for (const auto& seed : pda["seeds"].get_array()) {
            if (seed.is_object()) {
               auto seed_obj = seed.get_object();
               if (seed_obj.contains("kind")) {
                  std::string kind_str = seed_obj["kind"].as_string();
                  if (kind_str == "const" && seed_obj.contains("value")) {
                     acct.pda_seeds.emplace_back(pda_seed_kind::const_value,
                                                  fc::json::to_string(seed_obj["value"], fc::json::yield_function_t{}));
                  } else if (kind_str == "arg" && seed_obj.contains("path")) {
                     acct.pda_seeds.emplace_back(pda_seed_kind::arg, seed_obj["path"].as_string());
                  } else if (kind_str == "account" && seed_obj.contains("path")) {
                     acct.pda_seeds.emplace_back(pda_seed_kind::account, seed_obj["path"].as_string());
                  }
               }
            }
         }
      }
   }

   return acct;
}

instruction parse_instruction(const fc::variant& v) {
   instruction instr;
   auto obj = v.get_object();

   instr.name = obj["name"].as_string();

   // Parse discriminator if provided, otherwise compute it
   if (obj.contains("discriminator") && !obj["discriminator"].is_null()) {
      auto disc_arr = obj["discriminator"].get_array();
      FC_ASSERT(disc_arr.size() == 8, "Discriminator must be 8 bytes");
      for (size_t i = 0; i < 8; ++i) {
         instr.discriminator[i] = static_cast<uint8_t>(disc_arr[i].as_uint64());
      }
   } else {
      instr.compute_discriminator();
   }

   // Parse args
   if (obj.contains("args")) {
      for (const auto& arg : obj["args"].get_array()) {
         instr.args.push_back(parse_instruction_arg(arg));
      }
   }

   // Parse accounts
   if (obj.contains("accounts")) {
      for (const auto& acct : obj["accounts"].get_array()) {
         instr.accounts.push_back(parse_instruction_account(acct));
      }
   }

   // Parse returns (return type for view functions)
   if (obj.contains("returns") && !obj["returns"].is_null()) {
      instr.returns = idl_type::from_variant(obj["returns"]);
   }

   // Parse docs
   if (obj.contains("docs") && !obj["docs"].is_null()) {
      auto docs_arr = obj["docs"].get_array();
      if (!docs_arr.empty()) {
         std::string docs;
         for (const auto& d : docs_arr) {
            if (!docs.empty())
               docs += "\n";
            docs += d.as_string();
         }
         instr.docs = docs;
      }
   }

   return instr;
}

field parse_field(const fc::variant& v) {
   field f;
   auto obj = v.get_object();
   f.name = obj["name"].as_string();
   f.type = idl_type::from_variant(obj["type"]);
   return f;
}

account parse_account(const fc::variant& v) {
   account acct;
   auto obj = v.get_object();

   acct.name = obj["name"].as_string();

   // Parse discriminator if provided, otherwise compute it
   if (obj.contains("discriminator") && !obj["discriminator"].is_null()) {
      auto disc_arr = obj["discriminator"].get_array();
      FC_ASSERT(disc_arr.size() == 8, "Discriminator must be 8 bytes");
      for (size_t i = 0; i < 8; ++i) {
         acct.discriminator[i] = static_cast<uint8_t>(disc_arr[i].as_uint64());
      }
   } else {
      acct.compute_discriminator();
   }

   // Parse fields - handle both old and new IDL formats
   if (obj.contains("type") && obj["type"].is_object()) {
      auto type_obj = obj["type"].get_object();
      if (type_obj.contains("fields")) {
         for (const auto& f : type_obj["fields"].get_array()) {
            acct.fields.push_back(parse_field(f));
         }
      }
   }

   return acct;
}

event parse_event(const fc::variant& v) {
   event evt;
   auto obj = v.get_object();

   evt.name = obj["name"].as_string();

   if (obj.contains("fields")) {
      for (const auto& f : obj["fields"].get_array()) {
         evt.fields.push_back(parse_field(f));
      }
   }

   return evt;
}

error_def parse_error(const fc::variant& v) {
   error_def err;
   auto obj = v.get_object();

   err.code = static_cast<uint32_t>(obj["code"].as_uint64());
   err.name = obj["name"].as_string();
   if (obj.contains("msg"))
      err.msg = obj["msg"].as_string();

   return err;
}

enum_variant parse_enum_variant(const fc::variant& v) {
   enum_variant variant;
   auto obj = v.get_object();

   variant.name = obj["name"].as_string();

   if (obj.contains("fields") && !obj["fields"].is_null()) {
      std::vector<field> fields;
      for (const auto& f : obj["fields"].get_array()) {
         fields.push_back(parse_field(f));
      }
      variant.fields = std::move(fields);
   }

   return variant;
}

type_def parse_type_def(const fc::variant& v) {
   type_def def;
   auto obj = v.get_object();

   def.name = obj["name"].as_string();

   if (obj.contains("type") && obj["type"].is_object()) {
      auto type_obj = obj["type"].get_object();

      if (type_obj.contains("kind")) {
         std::string kind = type_obj["kind"].as_string();
         if (kind == "struct") {
            std::vector<field> fields;
            if (type_obj.contains("fields")) {
               for (const auto& f : type_obj["fields"].get_array()) {
                  fields.push_back(parse_field(f));
               }
            }
            def.struct_fields = std::move(fields);
         } else if (kind == "enum") {
            std::vector<enum_variant> variants;
            if (type_obj.contains("variants")) {
               for (const auto& var : type_obj["variants"].get_array()) {
                  variants.push_back(parse_enum_variant(var));
               }
            }
            def.enum_variants = std::move(variants);
         }
      }
   }

   return def;
}

}  // namespace

program parse_idl(const fc::variant& json) {
   program prog;
   auto obj = json.get_object();

   // Parse name and version
   if (obj.contains("name"))
      prog.name = obj["name"].as_string();
   if (obj.contains("version"))
      prog.version = obj["version"].as_string();

   // Parse instructions
   if (obj.contains("instructions")) {
      for (const auto& instr : obj["instructions"].get_array()) {
         prog.instructions.push_back(parse_instruction(instr));
      }
   }

   // Parse accounts
   if (obj.contains("accounts")) {
      for (const auto& acct : obj["accounts"].get_array()) {
         prog.accounts.push_back(parse_account(acct));
      }
   }

   // Parse events
   if (obj.contains("events")) {
      for (const auto& evt : obj["events"].get_array()) {
         prog.events.push_back(parse_event(evt));
      }
   }

   // Parse errors
   if (obj.contains("errors")) {
      for (const auto& err : obj["errors"].get_array()) {
         prog.errors.push_back(parse_error(err));
      }
   }

   // Parse types
   if (obj.contains("types")) {
      for (const auto& t : obj["types"].get_array()) {
         prog.types.push_back(parse_type_def(t));
      }
   }

   // Parse docs
   if (obj.contains("docs") && !obj["docs"].is_null()) {
      auto docs_arr = obj["docs"].get_array();
      if (!docs_arr.empty()) {
         std::string docs;
         for (const auto& d : docs_arr) {
            if (!docs.empty())
               docs += "\n";
            docs += d.as_string();
         }
         prog.docs = docs;
      }
   }

   return prog;
}

program parse_idl_file(const std::string& path) {
   std::ifstream file(path);
   FC_ASSERT(file.is_open(), "Failed to open IDL file: {}", path);

   std::stringstream buffer;
   buffer << file.rdbuf();
   std::string json_str = buffer.str();

   fc::variant json = fc::json::from_string(json_str);
   return parse_idl(json);
}

//=============================================================================
// Discriminator computation
//=============================================================================

std::array<uint8_t, 8> compute_instruction_discriminator(const std::string& name) {
   return borsh::compute_discriminator("global", name);
}

std::array<uint8_t, 8> compute_account_discriminator(const std::string& name) {
   return borsh::compute_discriminator("account", name);
}

std::array<uint8_t, 8> compute_state_discriminator(const std::string& name) {
   return borsh::compute_discriminator("state", name);
}

}  // namespace fc::network::solana::idl
