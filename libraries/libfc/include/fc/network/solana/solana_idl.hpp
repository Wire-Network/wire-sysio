// SPDX-License-Identifier: MIT
#pragma once

#include <fc/network/solana/solana_types.hpp>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <fc/reflect/reflect.hpp>
#include <fc/variant.hpp>
#include <magic_enum/magic_enum.hpp>

namespace fc::network::solana::idl {

/**
 * @brief Primitive types supported in Solana/Anchor IDL
 */
enum class primitive_type {
   // Boolean
   bool_t,
   // Unsigned integers
   u8,
   u16,
   u32,
   u64,
   u128,
   u256,
   // Signed integers
   i8,
   i16,
   i32,
   i64,
   i128,
   i256,
   // Floating point
   f32,
   f64,
   // String and bytes
   string,
   bytes,
   // Solana pubkey (can be "publicKey" or "pubkey" in IDL)
   pubkey
};

/**
 * @brief Convert a string to primitive_type
 * @return The primitive type, or nullopt if not a valid primitive
 */
std::optional<primitive_type> primitive_type_from_string(std::string_view s);

/**
 * @brief Convert primitive_type to string representation used in IDL
 */
std::string_view primitive_type_to_string(primitive_type t);

/**
 * @brief Kind of IDL type (discriminant for the type union)
 */
enum class type_kind {
   primitive,  // A primitive type like u64, string, pubkey
   defined,    // A user-defined type (struct/enum)
   option,     // Option<T>
   vec,        // Vec<T>
   array,      // [T; N]
   tuple       // (T1, T2, ...)
};

/**
 * @brief PDA seed kind for account derivation
 */
enum class pda_seed_kind {
   const_value,  // Constant value seed
   arg,          // Instruction argument seed
   account       // Account seed
};

/**
 * @brief PDA seed definition
 */
struct pda_seed {
   pda_seed_kind kind;
   std::string path;  // For arg/account: the path to the value; for const: JSON value

   pda_seed() = default;
   pda_seed(pda_seed_kind k, std::string p) : kind(k), path(std::move(p)) {}
};

/**
 * @brief IDL type representation
 *
 * Represents a type in an Anchor IDL. Can be a primitive type, a defined type,
 * an option, a vector, an array, or a tuple.
 */
struct idl_type {
   // The kind of type
   type_kind kind = type_kind::primitive;

   // For primitive types - which primitive
   std::optional<primitive_type> primitive;

   // For custom/defined types - the name of the defined type
   std::optional<std::string> defined_name;

   // For Option<T> - the inner type
   std::shared_ptr<idl_type> option_inner;

   // For Vec<T> - the element type
   std::shared_ptr<idl_type> vec_element;

   // For fixed arrays [T; N] - the array length and element type
   std::optional<size_t> array_len;
   std::shared_ptr<idl_type> array_element;

   // For tuple types - the element types
   std::optional<std::vector<idl_type>> tuple_elements;

   idl_type() = default;

   /**
    * @brief Create a primitive type
    */
   static idl_type make_primitive(primitive_type p);

   /**
    * @brief Create a defined (custom) type
    */
   static idl_type make_defined(std::string name);

   /**
    * @brief Create an Option<T> type
    */
   static idl_type make_option(idl_type inner);

   /**
    * @brief Create a Vec<T> type
    */
   static idl_type make_vec(idl_type element);

   /**
    * @brief Create an array [T; N] type
    */
   static idl_type make_array(idl_type element, size_t len);

   /**
    * @brief Create a tuple type
    */
   static idl_type make_tuple(std::vector<idl_type> elements);

   /**
    * @brief Check if this is a primitive type
    */
   bool is_primitive() const { return kind == type_kind::primitive; }

   /**
    * @brief Check if this is a defined (custom) type
    */
   bool is_defined() const { return kind == type_kind::defined; }

   /**
    * @brief Check if this is an Option type
    */
   bool is_option() const { return kind == type_kind::option; }

   /**
    * @brief Check if this is a Vec type
    */
   bool is_vec() const { return kind == type_kind::vec; }

   /**
    * @brief Check if this is a fixed array type
    */
   bool is_array() const { return kind == type_kind::array; }

   /**
    * @brief Check if this is a tuple type
    */
   bool is_tuple() const { return kind == type_kind::tuple; }

   /**
    * @brief Get the primitive type (throws if not primitive)
    */
   primitive_type get_primitive() const;

   /**
    * @brief Get the defined type name (throws if not defined)
    */
   const std::string& get_defined_name() const;

   /**
    * @brief Parse an IDL type from JSON variant
    */
   static idl_type from_variant(const fc::variant& v);

   /**
    * @brief Convert to string representation for debugging/display
    */
   std::string to_string() const;
};

/**
 * @brief Instruction argument definition
 */
struct instruction_arg {
   std::string name;
   idl_type type;
};

/**
 * @brief Account requirement for an instruction
 */
struct instruction_account {
   std::string name;
   bool is_mut = false;
   bool is_signer = false;
   bool is_optional = false;

   // For known addresses (like system program)
   std::optional<pubkey> address;

   // PDA seed components for derivation
   std::vector<pda_seed> pda_seeds;

   // Documentation
   std::optional<std::string> docs;
};

/**
 * @brief Instruction definition from IDL
 */
struct instruction {
   std::string name;

   // Anchor 8-byte discriminator (first 8 bytes of sha256("global:<name>"))
   std::array<uint8_t, 8> discriminator{};

   // Instruction arguments
   std::vector<instruction_arg> args;

   // Required accounts
   std::vector<instruction_account> accounts;

   // Documentation
   std::optional<std::string> docs;

   /**
    * @brief Compute and set the discriminator based on instruction name
    */
   void compute_discriminator();
};

/**
 * @brief Field definition for accounts and structs
 */
struct field {
   std::string name;
   idl_type type;
};

/**
 * @brief Account type definition from IDL
 */
struct account {
   std::string name;

   // Anchor 8-byte discriminator (first 8 bytes of sha256("account:<name>"))
   std::array<uint8_t, 8> discriminator{};

   // Account fields
   std::vector<field> fields;

   /**
    * @brief Compute and set the discriminator based on account name
    */
   void compute_discriminator();
};

/**
 * @brief Event definition from IDL
 */
struct event {
   std::string name;
   std::vector<field> fields;
};

/**
 * @brief Error code definition from IDL
 */
struct error_def {
   uint32_t code = 0;
   std::string name;
   std::string msg;
};

/**
 * @brief Enum variant definition
 */
struct enum_variant {
   std::string name;
   // Optional fields for tuple variants
   std::optional<std::vector<field>> fields;
};

/**
 * @brief Custom type definition (struct or enum)
 */
struct type_def {
   std::string name;

   // For structs - the fields
   std::optional<std::vector<field>> struct_fields;

   // For enums - the variants
   std::optional<std::vector<enum_variant>> enum_variants;

   bool is_struct() const { return struct_fields.has_value(); }
   bool is_enum() const { return enum_variants.has_value(); }
};

/**
 * @brief Complete IDL program definition
 */
struct program {
   std::string name;
   std::string version;

   // Program instructions
   std::vector<instruction> instructions;

   // Account types
   std::vector<account> accounts;

   // Events
   std::vector<event> events;

   // Errors
   std::vector<error_def> errors;

   // Custom types
   std::vector<type_def> types;

   // Program metadata
   std::optional<std::string> docs;

   /**
    * @brief Find an instruction by name
    */
   const instruction* find_instruction(const std::string& name) const;

   /**
    * @brief Find an account type by name
    */
   const account* find_account(const std::string& name) const;

   /**
    * @brief Find a type definition by name
    */
   const type_def* find_type(const std::string& name) const;

   /**
    * @brief Find an error by code
    */
   const error_def* find_error(uint32_t code) const;
};

/**
 * @brief Parse an IDL from JSON variant
 */
program parse_idl(const fc::variant& json);

/**
 * @brief Parse an IDL from a JSON file
 */
program parse_idl_file(const std::string& path);

/**
 * @brief Compute Anchor discriminator for instruction
 *
 * Returns the first 8 bytes of sha256("global:<name>")
 */
std::array<uint8_t, 8> compute_instruction_discriminator(const std::string& name);

/**
 * @brief Compute Anchor discriminator for account
 *
 * Returns the first 8 bytes of sha256("account:<name>")
 */
std::array<uint8_t, 8> compute_account_discriminator(const std::string& name);

/**
 * @brief Compute Anchor discriminator for state method
 *
 * Returns the first 8 bytes of sha256("state:<name>")
 */
std::array<uint8_t, 8> compute_state_discriminator(const std::string& name);

}  // namespace fc::network::solana::idl

// Reflection macros for enum classes
FC_REFLECT_ENUM(fc::network::solana::idl::primitive_type,
                (bool_t)(u8)(u16)(u32)(u64)(u128)(u256)(i8)(i16)(i32)(i64)(i128)(i256)(f32)(f64)(string)(bytes)(pubkey))
FC_REFLECT_ENUM(fc::network::solana::idl::type_kind, (primitive)(defined)(option)(vec)(array)(tuple))
FC_REFLECT_ENUM(fc::network::solana::idl::pda_seed_kind, (const_value)(arg)(account))

// Reflection macros for structs
FC_REFLECT(fc::network::solana::idl::pda_seed, (kind)(path))
FC_REFLECT(fc::network::solana::idl::idl_type, (kind)(primitive)(defined_name)(array_len)(tuple_elements))
FC_REFLECT(fc::network::solana::idl::instruction_arg, (name)(type))
FC_REFLECT(fc::network::solana::idl::instruction_account,
           (name)(is_mut)(is_signer)(is_optional)(address)(pda_seeds)(docs))
FC_REFLECT(fc::network::solana::idl::instruction, (name)(discriminator)(args)(accounts)(docs))
FC_REFLECT(fc::network::solana::idl::field, (name)(type))
FC_REFLECT(fc::network::solana::idl::account, (name)(discriminator)(fields))
FC_REFLECT(fc::network::solana::idl::event, (name)(fields))
FC_REFLECT(fc::network::solana::idl::error_def, (code)(name)(msg))
FC_REFLECT(fc::network::solana::idl::enum_variant, (name)(fields))
FC_REFLECT(fc::network::solana::idl::type_def, (name)(struct_fields)(enum_variants))
FC_REFLECT(fc::network::solana::idl::program, (name)(version)(instructions)(accounts)(events)(errors)(types)(docs))
