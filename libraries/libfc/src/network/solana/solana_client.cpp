// SPDX-License-Identifier: MIT
#include <fc/network/solana/solana_client.hpp>

#include <fc/crypto/base64.hpp>
#include <fc/int256.hpp>
#include <fc/io/json.hpp>
#include <fc/log/logger.hpp>
#include <fc/network/solana/solana_borsh.hpp>

#include <thread>

namespace fc::network::solana {

//=============================================================================
// solana_program_data_client implementation
//=============================================================================

solana_program_data_client::solana_program_data_client(const solana_client_ptr& client, const pubkey& program_id)
   : program_id(program_id), client(client) {}

instruction solana_program_data_client::build_instruction(const instruction_data_t& data,
                                                          const std::vector<account_meta>& accounts) {
   instruction instr;
   instr.program_id = program_id;
   instr.accounts = accounts;
   instr.data = data;
   return instr;
}

std::vector<uint8_t> solana_program_data_client::call(const instruction_data_t& data,
                                                      const std::vector<account_meta>& accounts,
                                                      commitment_t commitment) {
   auto instr = build_instruction(data, accounts);
   auto tx = client->create_transaction({instr}, client->get_pubkey());

   // Simulate the transaction
   auto sim_result = client->simulate_transaction(tx, commitment);

   // Extract return data from simulation result
   auto result_obj = sim_result.get_object();
   if (result_obj.contains("value") && !result_obj["value"].is_null()) {
      auto value_obj = result_obj["value"].get_object();

      // Check for simulation error
      if (value_obj.contains("err") && !value_obj["err"].is_null()) {
         FC_THROW("Transaction simulation failed: ${err}",
                  ("err", fc::json::to_string(value_obj["err"], fc::json::yield_function_t{})));
      }

      // Extract return data if present
      if (value_obj.contains("returnData") && !value_obj["returnData"].is_null()) {
         auto return_data_obj = value_obj["returnData"].get_object();
         if (return_data_obj.contains("data") && !return_data_obj["data"].is_null()) {
            auto data_arr = return_data_obj["data"].get_array();
            if (!data_arr.empty() && data_arr[0].is_string()) {
               std::string data_b64 = data_arr[0].as_string();
               if (!data_b64.empty()) {
                  auto decoded = fc::base64_decode(data_b64);
                  return std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(decoded.data()),
                                              reinterpret_cast<const uint8_t*>(decoded.data()) + decoded.size());
               }
            }
         }
      }
   }

   return std::vector<uint8_t>{};
}

std::string solana_program_data_client::send_tx(const instruction_data_t& data,
                                                const std::vector<account_meta>& accounts) {
   auto instr = build_instruction(data, accounts);
   auto tx = client->create_transaction({instr}, client->get_pubkey());
   client->sign_transaction(tx);
   return client->send_transaction(tx);
}

std::string solana_program_data_client::send_and_confirm_tx(const instruction_data_t& data,
                                                            const std::vector<account_meta>& accounts,
                                                            commitment_t commitment) {
   auto instr = build_instruction(data, accounts);
   auto tx = client->create_transaction({instr}, client->get_pubkey());
   client->sign_transaction(tx);
   return client->send_and_confirm_transaction(tx, commitment);
}

solana_program_data_call_fn solana_program_data_client::create_call() {
   return [this](const instruction_data_t& data, const std::vector<account_meta>& accounts,
                 commitment_t commitment) -> std::vector<uint8_t> { return call(data, accounts, commitment); };
}

solana_program_data_tx_fn solana_program_data_client::create_tx() {
   return [this](const instruction_data_t& data, const std::vector<account_meta>& accounts) -> std::string {
      return send_tx(data, accounts);
   };
}

//=============================================================================
// solana_program_client implementation
//=============================================================================

solana_program_client::solana_program_client(const solana_client_ptr& client, const pubkey& program_id,
                                             const std::vector<idl::program>& idls)
   : program_id(program_id), client(client) {
   auto idl_map = _idl_map.writeable();
   for (const auto& idl : idls) {
      // Store the first IDL as the program reference for type lookups
      if (!_program) {
         _program = std::make_shared<idl::program>(idl);
      }
      for (const auto& instr : idl.instructions) {
         idl_map[instr.name] = instr;
      }
   }
}

bool solana_program_client::has_idl(const std::string& instruction_name) {
   return _idl_map.readable().contains(instruction_name);
}

const idl::instruction& solana_program_client::get_idl(const std::string& instruction_name) {
   return _idl_map.readable().at(instruction_name);
}

void solana_program_client::encode_type(borsh::encoder& encoder, const fc::variant& value,
                                         const idl::idl_type& type) {
   if (type.is_primitive()) {
      switch (type.get_primitive()) {
         case idl::primitive_type::bool_t:
            encoder.write_bool(value.as_bool());
            break;
         case idl::primitive_type::u8:
            encoder.write_u8(static_cast<uint8_t>(value.as_uint64()));
            break;
         case idl::primitive_type::u16:
            encoder.write_u16(static_cast<uint16_t>(value.as_uint64()));
            break;
         case idl::primitive_type::u32:
            encoder.write_u32(static_cast<uint32_t>(value.as_uint64()));
            break;
         case idl::primitive_type::u64:
            encoder.write_u64(value.as_uint64());
            break;
         case idl::primitive_type::u128:
            encoder.write_u128(fc::uint128(value.as_string()));
            break;
         case idl::primitive_type::u256:
            encoder.write_u256(fc::to_uint256(value));
            break;
         case idl::primitive_type::i8:
            encoder.write_i8(static_cast<int8_t>(value.as_int64()));
            break;
         case idl::primitive_type::i16:
            encoder.write_i16(static_cast<int16_t>(value.as_int64()));
            break;
         case idl::primitive_type::i32:
            encoder.write_i32(static_cast<int32_t>(value.as_int64()));
            break;
         case idl::primitive_type::i64:
            encoder.write_i64(value.as_int64());
            break;
         case idl::primitive_type::i128:
            encoder.write_i128(fc::int128(value.as_string()));
            break;
         case idl::primitive_type::i256:
            encoder.write_i256(fc::to_int256(value));
            break;
         case idl::primitive_type::f32:
            encoder.write_f32(static_cast<float>(value.as_double()));
            break;
         case idl::primitive_type::f64:
            encoder.write_f64(value.as_double());
            break;
         case idl::primitive_type::string:
            encoder.write_string(value.as_string());
            break;
         case idl::primitive_type::bytes: {
            if (value.is_blob()) {
               auto blob = value.as_blob();
               std::vector<uint8_t> bytes(blob.data.begin(), blob.data.end());
               encoder.write_bytes(bytes);
            } else if (value.is_array()) {
               // Array of u8 values
               auto arr = value.get_array();
               std::vector<uint8_t> bytes;
               bytes.reserve(arr.size());
               for (const auto& elem : arr) {
                  bytes.push_back(static_cast<uint8_t>(elem.as_uint64()));
               }
               encoder.write_bytes(bytes);
            } else {
               // Assume base64 encoded string
               auto decoded = fc::base64_decode(value.as_string());
               std::vector<uint8_t> bytes(reinterpret_cast<const uint8_t*>(decoded.data()),
                                          reinterpret_cast<const uint8_t*>(decoded.data()) + decoded.size());
               encoder.write_bytes(bytes);
            }
            break;
         }
         case idl::primitive_type::pubkey: {
            // Handle pubkey as base58 string or as object with data field
            if (value.is_string()) {
               encoder.write_pubkey(pubkey::from_base58(value.as_string()));
            } else if (value.is_object()) {
               auto obj = value.get_object();
               if (obj.contains("data")) {
                  // pubkey struct with data array
                  auto data_arr = obj["data"].get_array();
                  pubkey pk;
                  for (size_t i = 0; i < 32 && i < data_arr.size(); ++i) {
                     pk.data[i] = static_cast<uint8_t>(data_arr[i].as_uint64());
                  }
                  encoder.write_pubkey(pk);
               } else {
                  FC_THROW("Invalid pubkey object format");
               }
            } else {
               FC_THROW("Invalid pubkey value type");
            }
            break;
         }
         default:
            FC_THROW("Unsupported primitive type: ${t}", ("t", static_cast<int>(type.get_primitive())));
      }
   } else if (type.is_option()) {
      if (value.is_null()) {
         encoder.write_u8(0);  // None
      } else {
         encoder.write_u8(1);  // Some
         encode_type(encoder, value, *type.option_inner);
      }
   } else if (type.is_vec()) {
      FC_ASSERT(value.is_array(), "Expected array for Vec type");
      auto arr = value.get_array();
      encoder.write_u32(static_cast<uint32_t>(arr.size()));
      for (const auto& elem : arr) {
         encode_type(encoder, elem, *type.vec_element);
      }
   } else if (type.is_array()) {
      FC_ASSERT(value.is_array(), "Expected array for fixed array type");
      auto arr = value.get_array();
      FC_ASSERT(arr.size() == *type.array_len,
                "Array size mismatch: expected ${expected}, got ${actual}",
                ("expected", *type.array_len)("actual", arr.size()));
      for (const auto& elem : arr) {
         encode_type(encoder, elem, *type.array_element);
      }
   } else if (type.is_tuple()) {
      FC_ASSERT(value.is_array(), "Expected array for tuple type");
      auto arr = value.get_array();
      FC_ASSERT(arr.size() == type.tuple_elements->size(),
                "Tuple size mismatch: expected ${expected}, got ${actual}",
                ("expected", type.tuple_elements->size())("actual", arr.size()));
      for (size_t i = 0; i < arr.size(); ++i) {
         encode_type(encoder, arr[i], (*type.tuple_elements)[i]);
      }
   } else if (type.is_defined() && _program) {
      // Look up the type definition
      const idl::type_def* type_def = _program->find_type(type.get_defined_name());
      FC_ASSERT(type_def, "Type '${name}' not found in IDL", ("name", type.get_defined_name()));

      if (type_def->is_struct() && type_def->struct_fields) {
         encode_fields(encoder, value, *type_def->struct_fields);
      } else if (type_def->is_enum() && type_def->enum_variants) {
         // For enums, expect either:
         // 1. {"variant": "VariantName", "fields": {...}} format
         // 2. Just the variant name as string for unit variants
         if (value.is_string()) {
            // Find variant index by name
            std::string variant_name = value.as_string();
            for (size_t i = 0; i < type_def->enum_variants->size(); ++i) {
               if ((*type_def->enum_variants)[i].name == variant_name) {
                  encoder.write_u8(static_cast<uint8_t>(i));
                  return;
               }
            }
            FC_THROW("Unknown enum variant: ${name}", ("name", variant_name));
         } else if (value.is_object()) {
            auto obj = value.get_object();
            FC_ASSERT(obj.contains("variant"), "Enum object must have 'variant' field");
            std::string variant_name = obj["variant"].as_string();

            for (size_t i = 0; i < type_def->enum_variants->size(); ++i) {
               if ((*type_def->enum_variants)[i].name == variant_name) {
                  encoder.write_u8(static_cast<uint8_t>(i));
                  // Encode variant fields if present
                  const auto& variant = (*type_def->enum_variants)[i];
                  if (variant.fields && !variant.fields->empty()) {
                     FC_ASSERT(obj.contains("fields"), "Enum variant '${name}' requires fields",
                               ("name", variant_name));
                     encode_fields(encoder, obj["fields"], *variant.fields);
                  }
                  return;
               }
            }
            FC_THROW("Unknown enum variant: ${name}", ("name", variant_name));
         } else {
            FC_THROW("Invalid enum value format");
         }
      } else {
         FC_THROW("Type '${name}' is not a struct or enum", ("name", type.get_defined_name()));
      }
   } else if (type.is_defined()) {
      FC_THROW("Cannot encode defined type '${name}' without IDL program loaded",
               ("name", type.get_defined_name()));
   } else {
      FC_THROW("Cannot encode type: ${t}", ("t", type.to_string()));
   }
}

void solana_program_client::encode_fields(borsh::encoder& encoder, const fc::variant& value,
                                           const std::vector<idl::field>& fields) {
   FC_ASSERT(value.is_object(), "Expected object for struct/fields encoding");
   auto obj = value.get_object();

   for (auto& field : fields) {
      FC_ASSERT(obj.contains(field.name), "Missing required field: ${name}", ("name", field.name));
      encode_type(encoder, obj[field.name], field.type);
   }
}

std::vector<uint8_t> solana_program_client::build_instruction_data(const idl::instruction& instr,
                                                                   const program_invoke_data_items& params) {
   borsh::encoder encoder;

   // Write the 8-byte Anchor discriminator
   encoder.write_fixed_bytes(instr.discriminator.data(), instr.discriminator.size());

   // Encode parameters based on IDL argument types
   FC_ASSERT(params.size() <= instr.args.size(),
             "Too many parameters: expected at most ${expected}, got ${actual}",
             ("expected", instr.args.size())("actual", params.size()));

   for (size_t i = 0; i < params.size(); ++i) {
      encode_type(encoder, params[i], instr.args[i].type);
   }

   return encoder.finish();
}

instruction solana_program_client::build_instruction(const idl::instruction& instr,
                                                     const std::vector<account_meta>& accounts,
                                                     const program_invoke_data_items& params) {
   instruction result;
   result.program_id = program_id;
   result.accounts = accounts;
   result.data = build_instruction_data(instr, params);
   return result;
}

std::vector<uint8_t> solana_program_client::extract_return_data(const fc::variant& sim_result) {
   if (!sim_result.is_object())
      return {};

   auto result_obj = sim_result.get_object();
   if (!result_obj.contains("value") || result_obj["value"].is_null())
      return {};

   auto value_obj = result_obj["value"].get_object();

   // Check for simulation error
   if (value_obj.contains("err") && !value_obj["err"].is_null()) {
      FC_THROW("Transaction simulation failed: ${err}",
               ("err", fc::json::to_string(value_obj["err"], fc::json::yield_function_t{})));
   }

   // Extract return data if present
   if (value_obj.contains("returnData") && !value_obj["returnData"].is_null()) {
      auto return_data_obj = value_obj["returnData"].get_object();
      if (return_data_obj.contains("data") && !return_data_obj["data"].is_null()) {
         auto data_arr = return_data_obj["data"].get_array();
         if (!data_arr.empty() && data_arr[0].is_string()) {
            std::string data_b64 = data_arr[0].as_string();
            if (!data_b64.empty()) {
               auto decoded = fc::base64_decode(data_b64);
               return std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(decoded.data()),
                                           reinterpret_cast<const uint8_t*>(decoded.data()) + decoded.size());
            }
         }
      }
   }

   return {};
}

fc::variant solana_program_client::decode_type(borsh::decoder& decoder, const idl::idl_type& type) {
   if (type.is_primitive()) {
      switch (type.get_primitive()) {
         case idl::primitive_type::bool_t:
            return fc::variant(decoder.read_bool());
         case idl::primitive_type::u8:
            return fc::variant(static_cast<uint64_t>(decoder.read_u8()));
         case idl::primitive_type::u16:
            return fc::variant(static_cast<uint64_t>(decoder.read_u16()));
         case idl::primitive_type::u32:
            return fc::variant(static_cast<uint64_t>(decoder.read_u32()));
         case idl::primitive_type::u64:
            return fc::variant(decoder.read_u64());
         case idl::primitive_type::u128:
            return fc::variant(decoder.read_u128().str());
         case idl::primitive_type::u256:
            return fc::variant(decoder.read_u256().str());
         case idl::primitive_type::i8:
            return fc::variant(static_cast<int64_t>(decoder.read_i8()));
         case idl::primitive_type::i16:
            return fc::variant(static_cast<int64_t>(decoder.read_i16()));
         case idl::primitive_type::i32:
            return fc::variant(static_cast<int64_t>(decoder.read_i32()));
         case idl::primitive_type::i64:
            return fc::variant(decoder.read_i64());
         case idl::primitive_type::i128:
            return fc::variant(decoder.read_i128().str());
         case idl::primitive_type::i256:
            return fc::variant(decoder.read_i256().str());
         case idl::primitive_type::f32:
            return fc::variant(static_cast<double>(decoder.read_f32()));
         case idl::primitive_type::f64:
            return fc::variant(decoder.read_f64());
         case idl::primitive_type::string:
            return fc::variant(decoder.read_string());
         case idl::primitive_type::bytes: {
            auto bytes = decoder.read_bytes();
            return fc::variant(fc::base64_encode(reinterpret_cast<const char*>(bytes.data()),
                                                  static_cast<unsigned int>(bytes.size())));
         }
         case idl::primitive_type::pubkey:
            return fc::variant(decoder.read_pubkey().to_base58());
         default:
            FC_THROW("Unsupported primitive type: ${t}", ("t", static_cast<int>(type.get_primitive())));
      }
   } else if (type.is_option()) {
      uint8_t has_value = decoder.read_u8();
      if (has_value == 0) {
         return fc::variant();  // null
      }
      FC_ASSERT(has_value == 1, "Invalid option discriminator: ${v}", ("v", has_value));
      // Recursively decode the inner type
      return decode_type(decoder, *type.option_inner);
   } else if (type.is_vec()) {
      uint32_t len = decoder.read_u32();
      fc::variants arr;
      arr.reserve(len);

      // Decode each element
      for (uint32_t i = 0; i < len; ++i) {
         arr.push_back(decode_type(decoder, *type.vec_element));
      }
      return fc::variant(arr);
   } else if (type.is_array()) {
      fc::variants arr;
      arr.reserve(*type.array_len);

      // Decode each element (no length prefix for fixed arrays)
      for (size_t i = 0; i < *type.array_len; ++i) {
         arr.push_back(decode_type(decoder, *type.array_element));
      }
      return fc::variant(arr);
   } else if (type.is_tuple() && type.tuple_elements) {
      fc::variants arr;
      arr.reserve(type.tuple_elements->size());

      // Decode each tuple element
      for (const auto& elem_type : *type.tuple_elements) {
         arr.push_back(decode_type(decoder, elem_type));
      }
      return fc::variant(arr);
   } else if (type.is_defined()) {
      FC_ASSERT(_program, "Cannot decode defined type '${name}' without IDL program loaded",
                ("name", type.get_defined_name()));

      // Look up the type definition and decode
      const idl::type_def* type_def = _program->find_type(type.get_defined_name());
      FC_ASSERT(type_def, "Type '${name}' not found in IDL", ("name", type.get_defined_name()));

      if (type_def->is_struct() && type_def->struct_fields) {
         return decode_fields(decoder, *type_def->struct_fields);
      } else if (type_def->is_enum() && type_def->enum_variants) {
         // Decode enum variant index
         uint8_t variant_idx = decoder.read_u8();
         FC_ASSERT(variant_idx < type_def->enum_variants->size(),
                   "Invalid enum variant index ${idx} for type '${name}' (max: ${max})",
                   ("idx", variant_idx)("name", type.get_defined_name())("max", type_def->enum_variants->size() - 1));

         const auto& variant = (*type_def->enum_variants)[variant_idx];
         fc::mutable_variant_object obj;
         obj("variant", variant.name);

         // Decode variant fields if present
         if (variant.fields && !variant.fields->empty()) {
            obj("fields", decode_fields(decoder, *variant.fields));
         }
         return fc::variant(obj);
      } else {
         FC_THROW("Type '${name}' is neither a struct nor an enum", ("name", type.get_defined_name()));
      }
   }

   FC_THROW("Cannot decode type: ${t}", ("t", type.to_string()));
}

fc::variant solana_program_client::decode_fields(borsh::decoder& decoder, const std::vector<idl::field>& fields) {
   fc::mutable_variant_object obj;
   for (const auto& field : fields) {
      obj(field.name, decode_type(decoder, field.type));
   }
   return fc::variant(obj);
}

fc::variant solana_program_client::decode_account_data(const std::vector<uint8_t>& data,
                                                        const std::string& account_name) {
   FC_ASSERT(_program, "No IDL program loaded");

   const idl::account* account = _program->find_account(account_name);
   FC_ASSERT(account, "Account type '${name}' not found in IDL", ("name", account_name));

   // Verify minimum size (8-byte discriminator + at least some data)
   FC_ASSERT(data.size() >= 8, "Account data too small: ${size} bytes", ("size", data.size()));

   // Verify discriminator
   for (size_t i = 0; i < 8; ++i) {
      FC_ASSERT(data[i] == account->discriminator[i],
                "Account discriminator mismatch at byte ${i}: expected ${exp}, got ${got}",
                ("i", i)("exp", static_cast<int>(account->discriminator[i]))("got", static_cast<int>(data[i])));
   }

   // Create decoder starting after discriminator
   borsh::decoder decoder(data.data() + 8, data.size() - 8);

   // Get fields - either from account definition or from corresponding type definition
   // In Anchor IDL format, the accounts section only has name/discriminator,
   // while the actual struct fields are defined in the types section with the same name
   const std::vector<idl::field>* fields = &account->fields;
   if (fields->empty()) {
      const idl::type_def* type_def = _program->find_type(account_name);
      FC_ASSERT(type_def, "Type definition '${name}' not found in IDL for account with no inline fields",
                ("name", account_name));
      FC_ASSERT(type_def->is_struct(), "Type '${name}' is not a struct", ("name", account_name));
      fields = &(*type_def->struct_fields);
   }

   // Decode all fields
   return decode_fields(decoder, *fields);
}

fc::variant solana_program_client::execute_call(const idl::instruction& instr,
                                                const std::vector<account_meta>& accounts,
                                                const program_invoke_data_items& params, commitment_t commitment) {
   auto instruction = build_instruction(instr, accounts, params);
   auto tx = client->create_transaction({instruction}, client->get_pubkey());

   // Simulate the transaction
   auto sim_result = client->simulate_transaction(tx, commitment);

   // Extract return data
   auto return_data = extract_return_data(sim_result);

   // If instruction has a return type, decode the data using IDL
   if (instr.returns.has_value() && !return_data.empty()) {
      borsh::decoder decoder(return_data);
      return decode_type(decoder, *instr.returns);
   }

   // If no return type specified but we have data, return as raw bytes (base64)
   if (!return_data.empty()) {
      return fc::variant(fc::base64_encode(reinterpret_cast<const char*>(return_data.data()),
                                            static_cast<unsigned int>(return_data.size())));
   }

   // No return data - return empty variant
   return fc::variant();
}

std::string solana_program_client::execute_tx(const idl::instruction& instr, const std::vector<account_meta>& accounts,
                                              const program_invoke_data_items& params) {
   auto instruction = build_instruction(instr, accounts, params);
   auto tx = client->create_transaction({instruction}, client->get_pubkey());
   client->sign_transaction(tx);
   return client->send_transaction(tx);
}

std::pair<pubkey, uint8_t> solana_program_client::derive_pda(const std::vector<idl::pda_seed>& pda_seeds,
                                                              const program_invoke_data_items& params) {
   std::vector<std::vector<uint8_t>> seeds;

   for (const auto& seed : pda_seeds) {
      switch (seed.kind) {
         case idl::pda_seed_kind::const_value: {
            // Parse the JSON value array into bytes
            auto value = fc::json::from_string(seed.path);
            if (value.is_array()) {
               std::vector<uint8_t> bytes;
               for (const auto& b : value.get_array()) {
                  bytes.push_back(static_cast<uint8_t>(b.as_uint64()));
               }
               seeds.push_back(std::move(bytes));
            } else if (value.is_string()) {
               // String constant
               auto str = value.as_string();
               seeds.push_back(std::vector<uint8_t>(str.begin(), str.end()));
            }
            break;
         }
         case idl::pda_seed_kind::arg: {
            // Use instruction argument as seed
            // path is the argument name or index
            // For now, try to match by index if numeric, otherwise search by name
            try {
               size_t idx = std::stoull(seed.path);
               if (idx < params.size()) {
                  auto& param = params[idx];
                  if (param.is_string()) {
                     auto str = param.as_string();
                     seeds.push_back(std::vector<uint8_t>(str.begin(), str.end()));
                  } else if (param.is_uint64()) {
                     // Encode as little-endian bytes
                     uint64_t val = param.as_uint64();
                     std::vector<uint8_t> bytes(8);
                     std::memcpy(bytes.data(), &val, 8);
                     seeds.push_back(std::move(bytes));
                  }
               }
            } catch (...) {
               // path is not an index, skip for now
               FC_THROW("Arg-based PDA seeds by name not yet supported: ${path}", ("path", seed.path));
            }
            break;
         }
         case idl::pda_seed_kind::account: {
            // Account-based seed - requires account resolution first
            FC_THROW("Account-based PDA seeds not yet supported: ${path}", ("path", seed.path));
         }
      }
   }

   return system::find_program_address(seeds, program_id);
}

std::vector<account_meta> solana_program_client::resolve_accounts(const idl::instruction& instr,
                                                                   const program_invoke_data_items& params,
                                                                   const account_overrides_t& account_overrides) {
   std::vector<account_meta> accounts;
   accounts.reserve(instr.accounts.size());

   for (const auto& acct : instr.accounts) {
      pubkey pk;
      bool resolved = false;

      // Check for explicit override first
      if (account_overrides.contains(acct.name)) {
         pk = account_overrides.at(acct.name);
         resolved = true;
      }
      // Check for fixed address in IDL
      else if (acct.address.has_value()) {
         pk = *acct.address;
         resolved = true;
      }
      // Derive PDA if seeds are provided
      else if (!acct.pda_seeds.empty()) {
         auto [pda, bump] = derive_pda(acct.pda_seeds, params);
         pk = pda;
         resolved = true;
      }
      // Signer accounts use the client's payer
      else if (acct.is_signer) {
         pk = client->get_pubkey();
         resolved = true;
      }

      FC_ASSERT(resolved, "Could not resolve account '${name}' - provide it in account_overrides",
                ("name", acct.name));

      // Create account_meta with correct flags
      if (acct.is_signer) {
         accounts.push_back(account_meta::signer(pk, acct.is_mut));
      } else if (acct.is_mut) {
         accounts.push_back(account_meta::writable(pk, false));
      } else {
         accounts.push_back(account_meta::readonly(pk, false));
      }
   }

   return accounts;
}

//=============================================================================
// solana_client implementation
//=============================================================================

solana_client::solana_client(const signature_provider_ptr& sig_provider,
                             const std::variant<std::string, fc::url>& url_source)
   : _signature_provider(sig_provider)
   , _pubkey(pubkey::from_public_key(_signature_provider->public_key))
   , _client(json_rpc_client::create(url_source)) {}

fc::variant solana_client::execute(const std::string& method, const fc::variant& params) {
   return _client.call(method, params);
}

fc::variant solana_client::build_config(commitment_t commitment, const std::optional<fc::variant_object>& extra) {
   fc::mutable_variant_object config;
   config("commitment", to_string(commitment));
   if (extra) {
      for (auto it = extra->begin(); it != extra->end(); ++it) {
         config(it->key(), it->value());
      }
   }
   return config;
}

//=============================================================================
// Account Methods
//=============================================================================

std::optional<account_info> solana_client::get_account_info(const pubkey_compat_t& address,
                                                             commitment_t commitment) {
   auto addr = to_pubkey(address);
   fc::mutable_variant_object config;
   config("commitment", to_string(commitment));
   config("encoding", "base64");

   fc::variants params{addr.to_base58(), config};
   auto result = execute("getAccountInfo", params);

   if (result.is_null() || !result.is_object())
      return std::nullopt;

   auto obj = result.get_object();
   if (!obj.contains("value") || obj["value"].is_null())
      return std::nullopt;

   auto value = obj["value"].get_object();
   account_info info;
   info.lamports = value["lamports"].as_uint64();
   info.owner = pubkey::from_base58(value["owner"].as_string());
   info.executable = value["executable"].as_bool();
   info.rent_epoch = value["rentEpoch"].as_uint64();

   // Decode base64 data
   auto data_arr = value["data"].get_array();
   if (!data_arr.empty() && data_arr[0].is_string()) {
      std::string data_b64 = data_arr[0].as_string();
      if (!data_b64.empty()) {
         auto decoded = fc::base64_decode(data_b64);
         info.data.assign(reinterpret_cast<const uint8_t*>(decoded.data()),
                          reinterpret_cast<const uint8_t*>(decoded.data()) + decoded.size());
      }
   }

   return info;
}

uint64_t solana_client::get_balance(const pubkey_compat_t& address, commitment_t commitment) {
   auto addr = to_pubkey(address);
   fc::variants params{addr.to_base58(), build_config(commitment)};
   auto result = execute("getBalance", params);

   auto obj = result.get_object();
   return obj["value"].as_uint64();
}

std::vector<std::optional<account_info>>
solana_client::get_multiple_accounts(const std::vector<pubkey>& addresses, commitment_t commitment) {
   fc::variants addr_list;
   for (const auto& addr : addresses) {
      addr_list.push_back(addr.to_base58());
   }

   fc::mutable_variant_object config;
   config("commitment", to_string(commitment));
   config("encoding", "base64");

   fc::variants params{addr_list, config};
   auto result = execute("getMultipleAccounts", params);

   std::vector<std::optional<account_info>> results;
   auto value_arr = result.get_object()["value"].get_array();

   for (const auto& v : value_arr) {
      if (v.is_null()) {
         results.push_back(std::nullopt);
         continue;
      }

      auto value = v.get_object();
      account_info info;
      info.lamports = value["lamports"].as_uint64();
      info.owner = pubkey::from_base58(value["owner"].as_string());
      info.executable = value["executable"].as_bool();
      info.rent_epoch = value["rentEpoch"].as_uint64();

      auto data_arr = value["data"].get_array();
      if (!data_arr.empty() && data_arr[0].is_string()) {
         std::string data_b64 = data_arr[0].as_string();
         if (!data_b64.empty()) {
            auto decoded = fc::base64_decode(data_b64);
            info.data.assign(reinterpret_cast<const uint8_t*>(decoded.data()),
                             reinterpret_cast<const uint8_t*>(decoded.data()) + decoded.size());
         }
      }

      results.push_back(info);
   }

   return results;
}

//=============================================================================
// Block Methods
//=============================================================================

uint64_t solana_client::get_block_height(commitment_t commitment) {
   fc::variants params{build_config(commitment)};
   return execute("getBlockHeight", params).as_uint64();
}

fc::variant solana_client::get_block(uint64_t slot, commitment_t commitment) {
   fc::mutable_variant_object config;
   config("commitment", to_string(commitment));
   config("encoding", "json");
   config("transactionDetails", "full");
   config("rewards", true);

   fc::variants params{slot, config};
   return execute("getBlock", params);
}

fc::variant solana_client::get_block_commitment(uint64_t slot) {
   fc::variants params{slot};
   return execute("getBlockCommitment", params);
}

std::vector<uint64_t> solana_client::get_blocks(uint64_t start_slot, std::optional<uint64_t> end_slot) {
   fc::variants params{start_slot};
   if (end_slot)
      params.push_back(*end_slot);

   auto result = execute("getBlocks", params);
   std::vector<uint64_t> blocks;
   for (const auto& b : result.get_array()) {
      blocks.push_back(b.as_uint64());
   }
   return blocks;
}

std::vector<uint64_t> solana_client::get_blocks_with_limit(uint64_t start_slot, uint64_t limit) {
   fc::variants params{start_slot, limit};
   auto result = execute("getBlocksWithLimit", params);

   std::vector<uint64_t> blocks;
   for (const auto& b : result.get_array()) {
      blocks.push_back(b.as_uint64());
   }
   return blocks;
}

std::optional<int64_t> solana_client::get_block_time(uint64_t slot) {
   fc::variants params{slot};
   auto result = execute("getBlockTime", params);
   if (result.is_null())
      return std::nullopt;
   return result.as_int64();
}

//=============================================================================
// Blockhash Methods
//=============================================================================

blockhash_info solana_client::get_latest_blockhash(commitment_t commitment) {
   fc::variants params{build_config(commitment)};
   auto result = execute("getLatestBlockhash", params);

   auto value = result.get_object()["value"].get_object();
   blockhash_info info;
   info.blockhash = value["blockhash"].as_string();
   info.last_valid_block_height = value["lastValidBlockHeight"].as_uint64();
   return info;
}

bool solana_client::is_blockhash_valid(const std::string& blockhash, commitment_t commitment) {
   fc::variants params{blockhash, build_config(commitment)};
   auto result = execute("isBlockhashValid", params);
   return result.get_object()["value"].as_bool();
}

//=============================================================================
// Cluster Methods
//=============================================================================

fc::variant solana_client::get_cluster_nodes() {
   fc::variants params;
   return execute("getClusterNodes", params);
}

std::string solana_client::get_genesis_hash() {
   fc::variants params;
   return execute("getGenesisHash", params).as_string();
}

std::string solana_client::get_health() {
   fc::variants params;
   return execute("getHealth", params).as_string();
}

fc::variant solana_client::get_highest_snapshot_slot() {
   fc::variants params;
   return execute("getHighestSnapshotSlot", params);
}

std::string solana_client::get_identity() {
   fc::variants params;
   auto result = execute("getIdentity", params);
   return result.get_object()["identity"].as_string();
}

fc::variant solana_client::get_leader_schedule(std::optional<uint64_t> slot) {
   fc::variants params;
   if (slot)
      params.push_back(*slot);
   return execute("getLeaderSchedule", params);
}

uint64_t solana_client::get_slot(commitment_t commitment) {
   fc::variants params{build_config(commitment)};
   return execute("getSlot", params).as_uint64();
}

std::string solana_client::get_slot_leader(commitment_t commitment) {
   fc::variants params{build_config(commitment)};
   return execute("getSlotLeader", params).as_string();
}

std::vector<std::string> solana_client::get_slot_leaders(uint64_t start_slot, uint64_t limit) {
   fc::variants params{start_slot, limit};
   auto result = execute("getSlotLeaders", params);

   std::vector<std::string> leaders;
   for (const auto& l : result.get_array()) {
      leaders.push_back(l.as_string());
   }
   return leaders;
}

fc::variant solana_client::get_version() {
   fc::variants params;
   return execute("getVersion", params);
}

//=============================================================================
// Epoch Methods
//=============================================================================

fc::variant solana_client::get_epoch_info(commitment_t commitment) {
   fc::variants params{build_config(commitment)};
   return execute("getEpochInfo", params);
}

fc::variant solana_client::get_epoch_schedule() {
   fc::variants params;
   return execute("getEpochSchedule", params);
}

//=============================================================================
// Fee Methods
//=============================================================================

std::optional<uint64_t> solana_client::get_fee_for_message(const std::string& message_base64,
                                                            commitment_t commitment) {
   fc::variants params{message_base64, build_config(commitment)};
   auto result = execute("getFeeForMessage", params);

   auto value = result.get_object()["value"];
   if (value.is_null())
      return std::nullopt;
   return value.as_uint64();
}

std::vector<fc::variant> solana_client::get_recent_prioritization_fees(const std::vector<pubkey>& accounts) {
   fc::variants addr_list;
   for (const auto& addr : accounts) {
      addr_list.push_back(addr.to_base58());
   }

   fc::variants params;
   if (!addr_list.empty())
      params.push_back(addr_list);

   auto result = execute("getRecentPrioritizationFees", params);
   return result.get_array();
}

//=============================================================================
// Inflation Methods
//=============================================================================

fc::variant solana_client::get_inflation_governor(commitment_t commitment) {
   fc::variants params{build_config(commitment)};
   return execute("getInflationGovernor", params);
}

fc::variant solana_client::get_inflation_rate() {
   fc::variants params;
   return execute("getInflationRate", params);
}

fc::variant solana_client::get_inflation_reward(const std::vector<pubkey>& addresses,
                                                 std::optional<uint64_t> epoch) {
   fc::variants addr_list;
   for (const auto& addr : addresses) {
      addr_list.push_back(addr.to_base58());
   }

   fc::mutable_variant_object config;
   if (epoch)
      config("epoch", *epoch);

   fc::variants params{addr_list};
   if (epoch)
      params.push_back(config);

   return execute("getInflationReward", params);
}

//=============================================================================
// Supply Methods
//=============================================================================

fc::variant solana_client::get_supply(commitment_t commitment) {
   fc::variants params{build_config(commitment)};
   return execute("getSupply", params);
}

fc::variant solana_client::get_largest_accounts(commitment_t commitment) {
   fc::variants params{build_config(commitment)};
   return execute("getLargestAccounts", params);
}

//=============================================================================
// Stake Methods
//=============================================================================

uint64_t solana_client::get_stake_minimum_delegation(commitment_t commitment) {
   fc::variants params{build_config(commitment)};
   auto result = execute("getStakeMinimumDelegation", params);
   return result.get_object()["value"].as_uint64();
}

//=============================================================================
// Token Methods
//=============================================================================

fc::variant solana_client::get_token_account_balance(const pubkey_compat_t& token_account, commitment_t commitment) {
   auto addr = to_pubkey(token_account);
   fc::variants params{addr.to_base58(), build_config(commitment)};
   return execute("getTokenAccountBalance", params);
}

fc::variant solana_client::get_token_accounts_by_delegate(const pubkey_compat_t& delegate,
                                                           const fc::variant& filter, commitment_t commitment) {
   auto addr = to_pubkey(delegate);
   fc::mutable_variant_object config;
   config("commitment", to_string(commitment));
   config("encoding", "jsonParsed");

   fc::variants params{addr.to_base58(), filter, config};
   return execute("getTokenAccountsByDelegate", params);
}

fc::variant solana_client::get_token_accounts_by_owner(const pubkey_compat_t& owner, const fc::variant& filter,
                                                        commitment_t commitment) {
   auto addr = to_pubkey(owner);
   fc::mutable_variant_object config;
   config("commitment", to_string(commitment));
   config("encoding", "jsonParsed");

   fc::variants params{addr.to_base58(), filter, config};
   return execute("getTokenAccountsByOwner", params);
}

fc::variant solana_client::get_token_largest_accounts(const pubkey_compat_t& mint, commitment_t commitment) {
   auto addr = to_pubkey(mint);
   fc::variants params{addr.to_base58(), build_config(commitment)};
   return execute("getTokenLargestAccounts", params);
}

fc::variant solana_client::get_token_supply(const pubkey_compat_t& mint, commitment_t commitment) {
   auto addr = to_pubkey(mint);
   fc::variants params{addr.to_base58(), build_config(commitment)};
   return execute("getTokenSupply", params);
}

//=============================================================================
// Transaction Methods
//=============================================================================

fc::variant solana_client::get_transaction(const std::string& signature, commitment_t commitment) {
   fc::mutable_variant_object config;
   config("commitment", to_string(commitment));
   config("encoding", "json");

   fc::variants params{signature, config};
   return execute("getTransaction", params);
}

uint64_t solana_client::get_transaction_count(commitment_t commitment) {
   fc::variants params{build_config(commitment)};
   return execute("getTransactionCount", params).as_uint64();
}

std::vector<fc::variant> solana_client::get_signatures_for_address(const pubkey_compat_t& address,
                                                                    std::optional<std::string> before,
                                                                    std::optional<std::string> until, size_t limit) {
   auto addr = to_pubkey(address);
   fc::mutable_variant_object config;
   config("limit", limit);
   if (before)
      config("before", *before);
   if (until)
      config("until", *until);

   fc::variants params{addr.to_base58(), config};
   return execute("getSignaturesForAddress", params).get_array();
}

rpc_response<std::vector<std::optional<signature_status>>>
solana_client::get_signature_statuses(const std::vector<std::string>& signatures,
                                       bool search_transaction_history) {
   fc::variants sig_list;
   for (const auto& sig : signatures) {
      sig_list.push_back(sig);
   }

   fc::mutable_variant_object config;
   config("searchTransactionHistory", search_transaction_history);

   fc::variants params{sig_list, config};
   auto result = execute("getSignatureStatuses", params);

   rpc_response<std::vector<std::optional<signature_status>>> response;
   auto obj = result.get_object();
   response.context.slot = obj["context"].get_object()["slot"].as_uint64();

   for (const auto& v : obj["value"].get_array()) {
      if (v.is_null()) {
         response.value.push_back(std::nullopt);
         continue;
      }

      signature_status status;
      auto status_obj = v.get_object();
      status.slot = status_obj["slot"].as_uint64();

      if (status_obj.contains("confirmations") && !status_obj["confirmations"].is_null()) {
         status.confirmations = status_obj["confirmations"].as_uint64();
      }

      if (status_obj.contains("err") && !status_obj["err"].is_null()) {
         status.err = fc::json::to_string(status_obj["err"], fc::json::yield_function_t{});
      }

      if (status_obj.contains("confirmationStatus")) {
         status.confirmation_status = status_obj["confirmationStatus"].as_string();
      }

      response.value.push_back(status);
   }

   return response;
}

//=============================================================================
// Transaction Submission
//=============================================================================

std::string solana_client::send_transaction(const transaction& tx, bool skip_preflight,
                                             commitment_t preflight_commitment) {
   auto tx_bytes = tx.serialize();
   std::string tx_base64 = fc::base64_encode(reinterpret_cast<const char*>(tx_bytes.data()),
                                              static_cast<unsigned int>(tx_bytes.size()));
   return send_transaction(tx_base64, skip_preflight, preflight_commitment);
}

std::string solana_client::send_transaction(const std::string& tx_base64, bool skip_preflight,
                                             commitment_t preflight_commitment) {
   fc::mutable_variant_object config;
   config("encoding", "base64");
   config("skipPreflight", skip_preflight);
   config("preflightCommitment", to_string(preflight_commitment));

   fc::variants params{tx_base64, config};
   return execute("sendTransaction", params).as_string();
}

fc::variant solana_client::simulate_transaction(const transaction& tx, commitment_t commitment) {
   auto tx_bytes = tx.serialize();
   std::string tx_base64 = fc::base64_encode(reinterpret_cast<const char*>(tx_bytes.data()),
                                              static_cast<unsigned int>(tx_bytes.size()));

   fc::mutable_variant_object config;
   config("encoding", "base64");
   config("commitment", to_string(commitment));
   config("sigVerify", false);

   fc::variants params{tx_base64, config};
   return execute("simulateTransaction", params);
}

std::string solana_client::request_airdrop(const pubkey_compat_t& address, uint64_t lamports,
                                            commitment_t commitment) {
   auto addr = to_pubkey(address);
   fc::variants params{addr.to_base58(), lamports, build_config(commitment)};
   return execute("requestAirdrop", params).as_string();
}

//=============================================================================
// Program Methods
//=============================================================================

fc::variant solana_client::get_program_accounts(const pubkey_compat_t& program_id,
                                                 const std::vector<fc::variant>& filters, commitment_t commitment) {
   auto addr = to_pubkey(program_id);
   fc::mutable_variant_object config;
   config("commitment", to_string(commitment));
   config("encoding", "base64");

   if (!filters.empty()) {
      config("filters", filters);
   }

   fc::variants params{addr.to_base58(), config};
   return execute("getProgramAccounts", params);
}

//=============================================================================
// Vote Methods
//=============================================================================

fc::variant solana_client::get_vote_accounts(commitment_t commitment) {
   fc::variants params{build_config(commitment)};
   return execute("getVoteAccounts", params);
}

//=============================================================================
// Rent Methods
//=============================================================================

uint64_t solana_client::get_minimum_balance_for_rent_exemption(size_t data_length, commitment_t commitment) {
   fc::variants params{data_length, build_config(commitment)};
   return execute("getMinimumBalanceForRentExemption", params).as_uint64();
}

//=============================================================================
// Performance Methods
//=============================================================================

fc::variant solana_client::get_recent_performance_samples(size_t limit) {
   fc::variants params{limit};
   return execute("getRecentPerformanceSamples", params);
}

//=============================================================================
// Ledger Methods
//=============================================================================

uint64_t solana_client::get_first_available_block() {
   fc::variants params;
   return execute("getFirstAvailableBlock", params).as_uint64();
}

uint64_t solana_client::minimum_ledger_slot() {
   fc::variants params;
   return execute("minimumLedgerSlot", params).as_uint64();
}

uint64_t solana_client::get_max_retransmit_slot() {
   fc::variants params;
   return execute("getMaxRetransmitSlot", params).as_uint64();
}

uint64_t solana_client::get_max_shred_insert_slot() {
   fc::variants params;
   return execute("getMaxShredInsertSlot", params).as_uint64();
}

//=============================================================================
// Block Production
//=============================================================================

fc::variant solana_client::get_block_production(std::optional<uint64_t> first_slot,
                                                 std::optional<uint64_t> last_slot) {
   fc::mutable_variant_object config;
   if (first_slot || last_slot) {
      fc::mutable_variant_object range;
      if (first_slot)
         range("firstSlot", *first_slot);
      if (last_slot)
         range("lastSlot", *last_slot);
      config("range", range);
   }

   fc::variants params;
   if (first_slot || last_slot)
      params.push_back(config);

   return execute("getBlockProduction", params);
}

//=============================================================================
// Transaction Building Helpers
//=============================================================================

transaction solana_client::create_transaction(const std::vector<instruction>& instructions, const pubkey& fee_payer) {
   transaction tx;

   // Get a fresh blockhash
   auto bh_info = get_latest_blockhash();
   tx.msg.recent_blockhash = pubkey::from_base58(bh_info.blockhash);

   // Collect all unique accounts
   std::vector<account_meta> all_accounts;

   // Fee payer is always first and is a writable signer
   all_accounts.push_back(account_meta::signer(fee_payer, true));

   // Collect accounts from all instructions
   for (const auto& instr : instructions) {
      for (const auto& meta : instr.accounts) {
         // Check if already in list
         bool found = false;
         for (auto& existing : all_accounts) {
            if (existing.key == meta.key) {
               // Merge properties: is_signer and is_writable are "sticky" (once true, always true)
               existing.is_signer = existing.is_signer || meta.is_signer;
               existing.is_writable = existing.is_writable || meta.is_writable;
               found = true;
               break;
            }
         }
         if (!found) {
            all_accounts.push_back(meta);
         }
      }

      // Add program ID if not already present
      bool prog_found = false;
      for (const auto& existing : all_accounts) {
         if (existing.key == instr.program_id) {
            prog_found = true;
            break;
         }
      }
      if (!prog_found) {
         all_accounts.push_back(account_meta::readonly(instr.program_id, false));
      }
   }

   // Sort accounts: writable signers, readonly signers, writable non-signers, readonly non-signers
   std::vector<account_meta> writable_signers;
   std::vector<account_meta> readonly_signers;
   std::vector<account_meta> writable_non_signers;
   std::vector<account_meta> readonly_non_signers;

   for (const auto& meta : all_accounts) {
      if (meta.is_signer) {
         if (meta.is_writable)
            writable_signers.push_back(meta);
         else
            readonly_signers.push_back(meta);
      } else {
         if (meta.is_writable)
            writable_non_signers.push_back(meta);
         else
            readonly_non_signers.push_back(meta);
      }
   }

   // Build final account list
   tx.msg.account_keys.clear();
   for (const auto& m : writable_signers)
      tx.msg.account_keys.push_back(m.key);
   for (const auto& m : readonly_signers)
      tx.msg.account_keys.push_back(m.key);
   for (const auto& m : writable_non_signers)
      tx.msg.account_keys.push_back(m.key);
   for (const auto& m : readonly_non_signers)
      tx.msg.account_keys.push_back(m.key);

   // Set header
   tx.msg.header.num_required_signatures = writable_signers.size() + readonly_signers.size();
   tx.msg.header.num_readonly_signed_accounts = readonly_signers.size();
   tx.msg.header.num_readonly_unsigned_accounts = readonly_non_signers.size();

   // Build account key index map
   std::map<pubkey, uint8_t> key_index_map;
   for (size_t i = 0; i < tx.msg.account_keys.size(); ++i) {
      key_index_map[tx.msg.account_keys[i]] = static_cast<uint8_t>(i);
   }

   // Compile instructions
   for (const auto& instr : instructions) {
      compiled_instruction compiled;
      compiled.program_id_index = key_index_map[instr.program_id];

      for (const auto& meta : instr.accounts) {
         compiled.account_indices.push_back(key_index_map[meta.key]);
      }

      compiled.data = instr.data;
      tx.msg.instructions.push_back(compiled);
   }

   // Initialize empty signatures
   tx.signatures.resize(tx.msg.header.num_required_signatures);

   return tx;
}

transaction solana_client::sign_transaction(transaction& tx) {
   // Serialize the message for signing
   auto msg_bytes = tx.msg.serialize();

   // Solana signs the raw message bytes directly with ED25519
   // (ED25519 internally handles its own SHA-512 hashing as part of the EdDSA algorithm)
   FC_ASSERT(_signature_provider->private_key.has_value(), "Signature provider must have private key for signing");
   auto& priv_key = _signature_provider->private_key.value();
   FC_ASSERT(priv_key.contains<fc::crypto::ed::private_key_shim>(), "Private key must be ED25519 type for Solana");

   auto& ed_priv_key = priv_key.get<fc::crypto::ed::private_key_shim>();
   auto ed_sig = ed_priv_key.sign_raw(msg_bytes.data(), msg_bytes.size());

   // Find the fee payer's position (should be index 0)
   for (size_t i = 0; i < tx.msg.account_keys.size(); ++i) {
      if (tx.msg.account_keys[i] == _pubkey) {
         tx.signatures[i] = signature::from_ed_signature(ed_sig);
         break;
      }
   }

   return tx;
}

std::string solana_client::send_and_confirm_transaction(const transaction& tx, commitment_t commitment) {
   // Send the transaction
   std::string sig = send_transaction(tx, false, commitment);

   // Poll for confirmation
   const int max_retries = 60;  // 60 seconds max
   for (int i = 0; i < max_retries; ++i) {
      auto statuses = get_signature_statuses({sig}, false);
      if (!statuses.value.empty() && statuses.value[0].has_value()) {
         auto& status = *statuses.value[0];

         // Check for error
         if (status.err.has_value()) {
            FC_THROW("Transaction failed: ${err}", ("err", *status.err));
         }

         // Check if confirmed at requested level
         if (commitment == commitment_t::processed && !status.confirmation_status.empty()) {
            return sig;
         }
         if (commitment == commitment_t::confirmed &&
             (status.confirmation_status == "confirmed" || status.confirmation_status == "finalized")) {
            return sig;
         }
         if (commitment == commitment_t::finalized && status.confirmation_status == "finalized") {
            return sig;
         }
      }

      // Wait 1 second before next poll
      std::this_thread::sleep_for(std::chrono::seconds(1));
   }

   FC_THROW("Transaction confirmation timeout");
}

}  // namespace fc::network::solana
