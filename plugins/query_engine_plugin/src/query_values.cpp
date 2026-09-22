#include <fc/crypto/hex.hpp>
#include <fc/io/raw.hpp>
#include <fc/slug_name.hpp>
#include <sysio/chain/asset.hpp>
#include <sysio/chain/block_timestamp.hpp>
#include <sysio/query_engine_plugin/query.hpp>

#include <boost/date_time/posix_time/posix_time.hpp>

#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <array>
#include <set>
#include <tuple>

namespace sysio::query_engine {
namespace {
constexpr auto abi_boolean = "bool";
constexpr auto fixed_checksum = "fixed_bytes<32>";
constexpr auto field_amount = "amount";
constexpr auto field_symbol = "symbol";
constexpr auto field_precision = "precision";
constexpr auto field_contract = "contract";
constexpr auto ieee_prefix = "0x";
constexpr uint32_t byte_bits = 8;
constexpr uint32_t float32_bytes = 4;
constexpr uint32_t float64_bytes = 8;
constexpr uint32_t float128_bytes = 16;
constexpr uint32_t scalar_allocation_bytes = 512;
constexpr uint32_t node_allocation_factor = 2;
constexpr uint8_t sign_bit = 0x80;
constexpr auto epoch_date = "1970-01-01";
constexpr size_t timestamp_seconds_length = 19;
constexpr size_t timestamp_fraction_digits = 6;
using stream = fc::datastream<const char*>;
using namespace chain;

/// Use the chain's date codec with microsecond precision; fc::time_point's JSON codec truncates to milliseconds.
std::string format_time(int64_t microseconds) {
   const boost::posix_time::ptime epoch(boost::gregorian::from_simple_string(epoch_date));
   return boost::posix_time::to_iso_extended_string(epoch + boost::posix_time::microseconds(microseconds));
}

/// Parse UTC timestamps exactly, rejecting fractions that would be silently truncated by the date codec.
int64_t parse_time(std::string text) {
   if (text.ends_with('Z'))
      text.pop_back();
   if (text.size() < timestamp_seconds_length ||
       (text.size() > timestamp_seconds_length &&
        (text[timestamp_seconds_length] != '.' || text.size() == timestamp_seconds_length + 1 ||
         text.size() > timestamp_seconds_length + 1 + timestamp_fraction_digits)))
      throw query_error(error_kind::VALUE_ERROR, "Invalid timestamp literal");
   const boost::posix_time::ptime epoch(boost::gregorian::from_simple_string(epoch_date));
   return (boost::posix_time::from_iso_extended_string(text) - epoch).total_microseconds();
}

/// Reduce positive-denominator rationals before narrowing to the accumulator width.
void set_ratio(value& result, comparison_integer numerator, comparison_integer denominator) {
   if (denominator <= 0)
      throw query_error(error_kind::VALUE_ERROR, "Invalid numeric denominator");
   comparison_integer a = numerator < 0 ? -numerator : numerator;
   comparison_integer b = denominator;
   while (b != 0) {
      comparison_integer remainder = a % b;
      a = b;
      b = remainder;
   }
   if (a != 0) {
      numerator /= a;
      denominator /= a;
   }
   result.numerator = integer(numerator);
   result.denominator = integer(denominator);
}

/// Scalars whose canonical string orders by an underlying chain identifier.
bool is_name(primitive_type type) {
   return type == primitive_type::name || type == primitive_type::slug_name;
}

/// Preserve denomination in every arithmetic/comparison path.
void assert_units(const value& left, const value& right) {
   if (left.type != right.type || left.symbol != right.symbol || left.precision != right.precision ||
       left.contract != right.contract)
      throw query_error(error_kind::VALUE_ERROR, "Incompatible asset units");
}

/// Resolve a primitive's SQL family once while compiling the ABI.
logical_type primitive_logical(primitive_type type) {
   switch (type) {
   case primitive_type::boolean:
      return logical_type::boolean;
   case primitive_type::int8:
   case primitive_type::uint8:
   case primitive_type::int16:
   case primitive_type::uint16:
   case primitive_type::int32:
   case primitive_type::uint32:
   case primitive_type::int64:
   case primitive_type::uint64:
   case primitive_type::int128:
   case primitive_type::uint128:
   case primitive_type::varint32:
   case primitive_type::varuint32:
   case primitive_type::varint_int64:
   case primitive_type::varint_uint64:
      return logical_type::integer;
   case primitive_type::float32:
   case primitive_type::float64:
   case primitive_type::float128:
      return logical_type::ieee_hex;
   case primitive_type::asset:
      return logical_type::asset;
   case primitive_type::extended_asset:
      return logical_type::extended_asset;
   case primitive_type::time_point:
   case primitive_type::time_point_sec:
   case primitive_type::block_timestamp_type:
      return logical_type::time;
   default:
      return logical_type::text;
   }
}

/// Bounded descriptor compiler; recursive ABI graphs cannot reach the decoder.
class descriptor_compiler {
public:
   descriptor_compiler(const abi_def& abi, query_budget& budget)
      : abi(abi)
      , budget(budget) {}

   std::shared_ptr<const type_descriptor> create(std::string name, uint32_t depth = 0) {
      budget.check();
      budget.assert_limit(depth, constants::max_depth, "abi-depth");
      budget.assert_limit(++nodes, constants::max_ast_nodes, "abi-nodes");
      budget.charge_memory(sizeof(type_descriptor) * node_allocation_factor + name.size() * node_allocation_factor);
      if (!active.insert(name).second)
         throw query_error(error_kind::QUERY_SEMANTICS, "Recursive ABI types are unsupported");
      struct active_guard {
         std::set<std::string>& active;
         const std::string& name;
         ~active_guard() { active.erase(name); }
      } guard{active, name};
      auto result = std::make_shared<type_descriptor>();
      result->abi_type = name;
      if (name.ends_with("[]") || name.ends_with('?') || name.ends_with('$')) {
         const bool array = name.ends_with("[]");
         result->kind = array ? type_kind::array : name.ends_with('?') ? type_kind::optional : type_kind::extension;
         result->element = create(name.substr(0, name.size() - (array ? 2 : 1)), depth + 1);
         result->logical = array ? logical_type::json : result->element->logical;
         result->primitive = result->element->primitive;
         return result;
      }
      for (const auto& alias : abi.types) {
         if (alias.new_type_name != name)
            continue;
         *result = *create(alias.type, depth + 1);
         result->abi_type = name;
         return result;
      }
      if (name == fixed_checksum)
         name = std::string(magic_enum::enum_name(primitive_type::checksum256));
      const auto primitive =
         name == abi_boolean ? std::optional{primitive_type::boolean} : magic_enum::enum_cast<primitive_type>(name);
      if (primitive && *primitive != primitive_type::slug_name) {
         result->primitive = *primitive;
         result->logical = primitive_logical(*primitive);
         return result;
      }
      for (const auto& structure : abi.structs) {
         if (structure.name != name)
            continue;
         if (primitive == primitive_type::slug_name && structure.base.empty() && structure.fields.size() == 1 &&
             structure.fields.front().name == constants::value_namespace &&
             be_key_codec::resolve_key_type(abi, structure.fields.front().type) ==
                magic_enum::enum_name(primitive_type::uint64)) {
            result->primitive = primitive_type::slug_name;
            return result;
         }
         result->kind = type_kind::structure;
         result->logical = logical_type::json;
         if (!structure.base.empty()) {
            const auto base = create(structure.base, depth + 1);
            if (base->kind != type_kind::structure)
               throw query_error(error_kind::QUERY_SEMANTICS, "ABI base must be a struct");
            result->fields = base->fields;
         }
         std::set<std::string> names;
         for (const auto& field : result->fields)
            names.insert(field.name);
         for (const auto& field : structure.fields) {
            if (!names.insert(field.name).second)
               throw query_error(error_kind::QUERY_SEMANTICS, "Duplicate ABI field");
            budget.charge_memory(sizeof(field_descriptor) * node_allocation_factor + field.name.size());
            result->fields.push_back({field.name, create(field.type, depth + 1)});
         }
         return result;
      }
      for (const auto& variant : abi.variants.value) {
         if (variant.name != name)
            continue;
         result->kind = type_kind::variant;
         result->logical = logical_type::json;
         for (const auto& alternative : variant.types)
            result->alternatives.push_back(create(alternative, depth + 1));
         return result;
      }
      for (const auto& enumeration : abi.enums.value) {
         if (enumeration.name != name)
            continue;
         *result = *create(enumeration.type, depth + 1);
         result->abi_type = name;
         return result;
      }
      throw query_error(error_kind::QUERY_SEMANTICS, "Unsupported ABI type");
   }

private:
   const abi_def& abi;
   query_budget& budget;
   std::set<std::string> active;
   uint32_t nodes = 0;
};

/// Reuse raw codecs for fixed-size integer/chain primitives.
template <typename T>
T unpack(stream& input) {
   T result{};
   fc::raw::unpack(input, result);
   return result;
}

/// Preserve exact ABI asset quantities before any formatting.
value from_asset(const asset& source) {
   value result;
   result.null = false;
   result.type = logical_type::asset;
   result.primitive = primitive_type::asset;
   result.numerator = source.get_amount();
   result.denominator = source.precision();
   result.precision = source.decimals();
   result.symbol = source.symbol_name();
   return result;
}

/// Decode a bounded byte string after validating the size prefix against both input and budget.
std::string unpack_string(stream& input, query_budget& budget) {
   const auto size = unpack<fc::unsigned_int>(input).value;
   if (size > input.remaining())
      throw query_error(error_kind::ROW_DECODE_ERROR, "Invalid string length");
   budget.charge_memory(uint64_t(size) * node_allocation_factor);
   std::string result(size, '\0');
   input.read(result.data(), size);
   return result;
}

/// Read raw IEEE payloads without constructing a floating-point object.
std::string unpack_hex(stream& input, size_t bytes, query_budget& budget, bool ieee = false) {
   if (bytes > input.remaining())
      throw query_error(error_kind::ROW_DECODE_ERROR, "Truncated scalar");
   budget.charge_memory(bytes * (node_allocation_factor + 1) + sizeof(std::string));
   const auto* data = input.pos();
   input.skip(bytes);
   return (ieee ? ieee_prefix : "") + fc::to_hex(data, bytes);
}

/// Convert chain primitives to their existing canonical text representation.
template <typename T>
std::string unpack_text(stream& input) {
   fc::variant result;
   fc::to_variant(unpack<T>(input), result);
   return result.as_string();
}

/// Bound WebAuthn's variable payloads before its existing raw codec allocates strings or vectors.
template <typename T>
std::string unpack_crypto_text(stream& input, query_budget& budget) {
   auto probe = input;
   const auto tag = unpack<fc::unsigned_int>(probe).value;
   constexpr uint64_t text_allocation_factor = 16; // raw storage, base58 conversion and variant copies
   const auto skip = [&](size_t bytes) {
      if (bytes > probe.remaining())
         throw query_error(error_kind::ROW_DECODE_ERROR, "Truncated cryptographic value");
      probe.skip(bytes);
   };
   const auto variable = [&] {
      const auto bytes = unpack<fc::unsigned_int>(probe).value;
      skip(bytes);
      budget.charge_memory(uint64_t(bytes) * text_allocation_factor);
   };
   if constexpr (std::is_same_v<T, public_key_type>) {
      if (tag == magic_enum::enum_integer(public_key_type::key_type::wa)) {
         skip(sizeof(fc::crypto::webauthn::public_key::public_key_data_type) + sizeof(uint8_t));
         variable();
      }
   } else {
      if (tag == magic_enum::enum_integer(signature_type::sig_type::wa)) {
         skip(sizeof(fc::crypto::r1::compact_signature));
         variable();
         variable();
      }
   }
   return unpack_text<T>(input);
}

/// Decode one primitive exactly; every dynamic branch checks lengths before allocation.
value decode_primitive(const type_descriptor& type, stream& input, query_budget& budget) {
   budget.charge_memory(scalar_allocation_bytes);
   value result;
   result.null = false;
   result.type = type.logical;
   result.primitive = type.primitive;
   switch (type.primitive) {
   case primitive_type::boolean: {
      const auto raw = unpack<uint8_t>(input);
      if (raw > 1)
         throw query_error(error_kind::ROW_DECODE_ERROR, "Invalid boolean");
      result.numerator = raw;
      break;
   }
   case primitive_type::int8:
      result.numerator = unpack<int8_t>(input);
      break;
   case primitive_type::uint8:
      result.numerator = unpack<uint8_t>(input);
      break;
   case primitive_type::int16:
      result.numerator = unpack<int16_t>(input);
      break;
   case primitive_type::uint16:
      result.numerator = unpack<uint16_t>(input);
      break;
   case primitive_type::int32:
      result.numerator = unpack<int32_t>(input);
      break;
   case primitive_type::uint32:
      result.numerator = unpack<uint32_t>(input);
      break;
   case primitive_type::int64:
   case primitive_type::varint_int64:
      result.numerator = unpack<int64_t>(input);
      break;
   case primitive_type::uint64:
   case primitive_type::varint_uint64:
      result.numerator = unpack<uint64_t>(input);
      break;
   case primitive_type::int128:
   case primitive_type::uint128: {
      // Existing 128-bit raw codecs supply bits; decimal conversion uses integer arithmetic only.
      const auto bits = unpack<fc::uint128>(input);
      result.numerator = integer(static_cast<uint64_t>(bits >> (sizeof(uint64_t) * byte_bits)));
      result.numerator <<= sizeof(uint64_t) * byte_bits;
      result.numerator += static_cast<uint64_t>(bits);
      if (type.primitive == primitive_type::int128 &&
          (static_cast<uint64_t>(bits >> (sizeof(uint64_t) * byte_bits)) >> (sizeof(uint64_t) * byte_bits - 1)))
         result.numerator -= integer(1) << (sizeof(fc::uint128) * byte_bits);
      break;
   }
   case primitive_type::varint32:
      result.numerator = unpack<fc::signed_int>(input).value;
      break;
   case primitive_type::varuint32:
      result.numerator = unpack<fc::unsigned_int>(input).value;
      break;
   case primitive_type::float32:
      result.text = unpack_hex(input, float32_bytes, budget, true);
      break;
   case primitive_type::float64:
      result.text = unpack_hex(input, float64_bytes, budget, true);
      break;
   case primitive_type::float128:
      result.text = unpack_hex(input, float128_bytes, budget, true);
      break;
   case primitive_type::name: {
      const auto name = unpack<chain::name>(input);
      result.numerator = name.to_uint64_t();
      result.text = name.to_string();
      break;
   }
   case primitive_type::slug_name: {
      const auto name = fc::slug_name(unpack<uint64_t>(input));
      result.numerator = name.to_uint64_t();
      result.text = name.to_string();
      break;
   }
   case primitive_type::time_point: {
      const auto time = unpack<fc::time_point>(input);
      result.numerator = time.time_since_epoch().count();
      result.text = format_time(time.time_since_epoch().count());
      break;
   }
   case primitive_type::time_point_sec: {
      const auto time = unpack<fc::time_point_sec>(input);
      result.numerator = time.to_time_point().time_since_epoch().count();
      result.text = time.to_iso_string();
      break;
   }
   case primitive_type::block_timestamp_type: {
      const auto time = unpack<chain::block_timestamp_type>(input).to_time_point();
      result.numerator = time.time_since_epoch().count();
      result.text = time.to_iso_string();
      break;
   }
   case primitive_type::asset:
      result = from_asset(unpack<asset>(input));
      break;
   case primitive_type::extended_asset: {
      const auto asset = unpack<extended_asset>(input);
      result = from_asset(asset.quantity);
      result.type = logical_type::extended_asset;
      result.primitive = primitive_type::extended_asset;
      result.contract = asset.contract.to_string();
      break;
   }
   case primitive_type::string:
      result.text = unpack_string(input, budget);
      break;
   case primitive_type::bytes: {
      const auto size = unpack<fc::unsigned_int>(input).value;
      result.text = unpack_hex(input, size, budget);
      break;
   }
   case primitive_type::checksum160:
      result.text = unpack_text<checksum160_type>(input);
      break;
   case primitive_type::checksum256:
      result.text = unpack_text<checksum256_type>(input);
      break;
   case primitive_type::checksum512:
      result.text = unpack_text<checksum512_type>(input);
      break;
   case primitive_type::symbol:
      result.text = unpack_text<symbol>(input);
      break;
   case primitive_type::symbol_code:
      result.text = unpack_text<symbol_code>(input);
      break;
   case primitive_type::public_key:
      result.text = unpack_crypto_text<public_key_type>(input, budget);
      break;
   case primitive_type::signature:
      result.text = unpack_crypto_text<signature_type>(input, budget);
      break;
   case primitive_type::bitset: {
      const auto bits = unpack<fc::unsigned_int>(input).value;
      const auto blocks = fc::bitset::calc_num_blocks(bits);
      if (blocks > input.remaining())
         throw query_error(error_kind::ROW_DECODE_ERROR, "Invalid bitset length");
      budget.charge_memory(uint64_t(bits) + blocks);
      fc::bitset decoded(bits);
      for (size_t i = 0; i < blocks; ++i) {
         budget.check();
         decoded.byte(i) = unpack<uint8_t>(input);
      }
      if (!decoded.unused_bits_zeroed())
         throw query_error(error_kind::ROW_DECODE_ERROR, "Invalid bitset padding");
      result.text = decoded.to_string();
      break;
   }
   }
   return result;
}

/// Normalize a complete row recursively using precompiled, acyclic descriptors.
fc::variant decode_node(const type_descriptor& type, stream& input, query_budget& budget, uint32_t depth = 0) {
   budget.check();
   budget.assert_limit(depth, constants::max_depth, "abi-depth");
   budget.charge_memory(sizeof(fc::variant) * node_allocation_factor);
   switch (type.kind) {
   case type_kind::primitive:
      return to_cell(decode_primitive(type, input, budget), budget);
   case type_kind::optional: {
      const auto present = unpack<uint8_t>(input);
      if (present > 1)
         throw query_error(error_kind::ROW_DECODE_ERROR, "Invalid optional tag");
      return present ? decode_node(*type.element, input, budget, depth + 1) : fc::variant();
   }
   case type_kind::extension:
      return input.remaining() ? decode_node(*type.element, input, budget, depth + 1) : fc::variant();
   case type_kind::structure: {
      budget.charge_memory(type.fields.size() * sizeof(fc::variant) * node_allocation_factor);
      fc::mutable_variant_object result;
      for (const auto& field : type.fields) {
         budget.charge_memory(field.name.size());
         result(field.name, decode_node(*field.type, input, budget, depth + 1));
      }
      return result;
   }
   case type_kind::array: {
      const auto size = unpack<fc::unsigned_int>(input).value;
      budget.charge_memory(uint64_t(size) * sizeof(fc::variant) * node_allocation_factor);
      fc::variants result;
      result.reserve(size);
      for (uint32_t i = 0; i < size; ++i)
         result.push_back(decode_node(*type.element, input, budget, depth + 1));
      return result;
   }
   case type_kind::variant: {
      const auto index = unpack<fc::unsigned_int>(input).value;
      if (index >= type.alternatives.size())
         throw query_error(error_kind::ROW_DECODE_ERROR, "Invalid variant tag");
      const auto& alternative = *type.alternatives[index];
      budget.charge_memory(alternative.abi_type.size() + sizeof(fc::variant) * node_allocation_factor);
      return fc::variants{fc::variant(alternative.abi_type), decode_node(alternative, input, budget, depth + 1)};
   }
   }
   throw query_error(error_kind::ROW_DECODE_ERROR, "Invalid ABI descriptor");
}

/// Reconstruct a typed scalar from its already-normalized cell for field extraction.
value from_cell(const fc::variant& cell, const type_descriptor& type) {
   value result;
   result.type = type.logical;
   result.primitive = type.primitive;
   if (cell.is_null())
      return result;
   result.null = false;
   if (type.kind == type_kind::optional || type.kind == type_kind::extension)
      return from_cell(cell, *type.element);
   if (type.logical == logical_type::json) {
      result.container = cell;
      return result;
   }
   if (is_numeric(type.logical)) {
      result = parse_number(cell.as_string());
      result.type = type.logical;
   } else if (type.logical == logical_type::boolean)
      result.numerator = cell.as_bool() ? 1 : 0;
   else if (type.logical == logical_type::asset || type.logical == logical_type::extended_asset) {
      const auto& asset = cell.get_object();
      result = parse_number(asset[field_amount].as_string());
      result.type = type.logical;
      result.symbol = asset[field_symbol].as_string();
      result.precision = asset[field_precision].as_uint64();
      if (type.logical == logical_type::extended_asset)
         result.contract = asset[field_contract].as_string();
   } else {
      result.text = cell.as_string();
      if (type.logical == logical_type::time)
         result.type = logical_type::text;
      if (type.logical != logical_type::ieee_hex)
         result = coerce_literal(std::move(result), type);
   }
   result.primitive = type.primitive;
   return result;
}

/// Canonical key decode reuses the shared codec except for explicitly opaque IEEE leaves.
fc::variant decode_key_node(be_key_codec::reader& input, const be_key_codec::key_shape& shape,
                            const type_descriptor& type, query_budget& budget) {
   budget.check();
   budget.charge_memory(scalar_allocation_bytes);
   if (!shape.is_leaf) {
      // slug_name is an ABI struct backed by uint64, projected as its canonical name.
      if (type.primitive == primitive_type::slug_name && type.kind == type_kind::primitive) {
         const auto raw = be_key_codec::decode_shape(input, shape);
         value result;
         result.null = false;
         result.primitive = primitive_type::slug_name;
         const auto name = fc::slug_name(raw[constants::value_namespace].as_uint64());
         result.text = name.to_string();
         return to_cell(result, budget);
      }
      fc::mutable_variant_object result;
      if (shape.children.size() != type.fields.size())
         throw query_error(error_kind::ROW_DECODE_ERROR, "Key descriptor mismatch");
      for (size_t i = 0; i < shape.children.size(); ++i)
         result(shape.children[i].name, decode_key_node(input, shape.children[i], *type.fields[i].type, budget));
      return result;
   }
   if (type.logical == logical_type::ieee_hex) {
      const auto size = type.primitive == primitive_type::float32   ? float32_bytes
                        : type.primitive == primitive_type::float64 ? float64_bytes
                                                                    : float128_bytes;
      std::array<char, float128_bytes> bytes{};
      input.read_bytes(bytes.data(), size);
      if (static_cast<uint8_t>(bytes[0]) & sign_bit)
         bytes[0] ^= sign_bit;
      else
         for (size_t i = 0; i < size; ++i)
            bytes[i] = static_cast<char>(~static_cast<uint8_t>(bytes[i]));
      std::reverse(bytes.begin(), bytes.begin() + size);
      return std::string(ieee_prefix) + fc::to_hex(bytes.data(), size);
   }
   budget.charge_memory(input.remaining() * node_allocation_factor);
   const auto raw = be_key_codec::decode_field(input, shape.kind);
   if (type.logical == logical_type::integer) {
      if (raw.is_int64())
         return std::to_string(raw.as_int64());
      if (raw.is_uint64())
         return std::to_string(raw.as_uint64());
      return raw.as_string();
   }
   return raw;
}
} // namespace

bool is_numeric(logical_type type) {
   return type == logical_type::integer || type == logical_type::decimal;
}
bool is_ordered(logical_type type) {
   return type != logical_type::json && type != logical_type::ieee_hex;
}

encoding value_encoding(logical_type type) {
   switch (type) {
   case logical_type::integer:
   case logical_type::decimal:
      return encoding::decimal_string;
   case logical_type::boolean:
      return encoding::boolean;
   case logical_type::asset:
   case logical_type::extended_asset:
      return encoding::asset_object;
   case logical_type::json:
      return encoding::json;
   case logical_type::ieee_hex:
      return encoding::ieee_hex;
   default:
      return encoding::text;
   }
}

value parse_number(std::string_view text) {
   value result;
   result.type = logical_type::integer;
   result.null = false;
   bool negative = false;
   size_t position = 0;
   if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
      negative = text.front() == '-';
      ++position;
   }
   bool decimal = false;
   uint32_t scale = 0;
   size_t digits = 0;
   try {
      for (; position < text.size(); ++position) {
         const char character = text[position];
         if (character == '.' && !decimal && digits != 0) {
            decimal = true;
            continue;
         }
         if (character < '0' || character > '9')
            throw query_error(error_kind::VALUE_ERROR, "Invalid decimal");
         result.numerator *= constants::decimal_base;
         result.numerator += character - '0';
         ++digits;
         if (decimal) {
            if (++scale > constants::decimal_places)
               throw query_error(error_kind::VALUE_ERROR, "Decimal scale exceeds 18");
            result.denominator *= constants::decimal_base;
         }
      }
      if (!digits || (decimal && !scale))
         throw query_error(error_kind::VALUE_ERROR, "Invalid decimal");
      if (negative)
         result.numerator = -result.numerator;
      if (decimal)
         result.type = logical_type::decimal;
      set_ratio(result, comparison_integer(result.numerator), comparison_integer(result.denominator));
   } catch (const std::overflow_error&) {
      throw query_error(error_kind::VALUE_ERROR, "Numeric overflow");
   }
   return result;
}

std::string render_number(const value& source) {
   comparison_integer scale = 1;
   for (uint32_t i = 0; i < constants::decimal_places; ++i)
      scale *= constants::decimal_base;
   comparison_integer numerator = source.numerator;
   const bool negative = numerator < 0;
   if (negative)
      numerator = -numerator;
   numerator *= scale;
   comparison_integer rounded = numerator / source.denominator;
   const comparison_integer remainder = numerator % source.denominator;
   const comparison_integer twice = remainder * 2;
   if (twice > source.denominator || (twice == source.denominator && (rounded % 2) != 0))
      ++rounded;
   std::string digits = rounded.convert_to<std::string>();
   if (digits.size() <= constants::decimal_places)
      digits.insert(0, constants::decimal_places + 1 - digits.size(), '0');
   digits.insert(digits.size() - constants::decimal_places, 1, '.');
   while (digits.back() == '0')
      digits.pop_back();
   if (digits.back() == '.')
      digits.pop_back();
   if (negative && rounded != 0)
      digits.insert(digits.begin(), '-');
   return digits;
}

int compare_values(const value& left, const value& right) {
   if (left.null || right.null)
      throw query_error(error_kind::VALUE_ERROR, "Null comparison requires SQL logic");
   if (!is_ordered(left.type) || !is_ordered(right.type))
      throw query_error(error_kind::QUERY_SEMANTICS, "Container and IEEE values are projection-only");
   if (left.type == logical_type::asset || left.type == logical_type::extended_asset)
      assert_units(left, right);
   else if (!(is_numeric(left.type) && is_numeric(right.type)) && left.type != right.type)
      throw query_error(error_kind::QUERY_SEMANTICS, "Incompatible scalar types");
   if (left.type == logical_type::text && !is_name(left.primitive) && !is_name(right.primitive))
      return left.text.compare(right.text);
   if (left.type == logical_type::text && left.primitive != right.primitive)
      throw query_error(error_kind::QUERY_SEMANTICS, "Incompatible name types");
   const comparison_integer a = comparison_integer(left.numerator) * right.denominator;
   const comparison_integer b = comparison_integer(right.numerator) * left.denominator;
   return a < b ? -1 : a > b ? 1 : 0;
}

void add_value(value& destination, const value& source) {
   if (source.null)
      return;
   if (destination.null) {
      destination = source;
      return;
   }
   if (destination.type == logical_type::asset || destination.type == logical_type::extended_asset)
      assert_units(destination, source);
   else if (!is_numeric(destination.type) || !is_numeric(source.type))
      throw query_error(error_kind::QUERY_SEMANTICS, "SUM requires a numeric or asset field");
   try {
      set_ratio(destination,
                comparison_integer(destination.numerator) * source.denominator +
                   comparison_integer(source.numerator) * destination.denominator,
                comparison_integer(destination.denominator) * source.denominator);
   } catch (const std::overflow_error&) {
      throw query_error(error_kind::VALUE_ERROR, "Aggregate overflow");
   }
}

fc::variant to_cell(const value& source, query_budget& budget) {
   budget.check();
   if (source.null)
      return fc::variant();
   budget.charge_memory(scalar_allocation_bytes + source.text.size() + source.symbol.size() + source.contract.size());
   switch (source.type) {
   case logical_type::integer:
   case logical_type::decimal:
      return render_number(source);
   case logical_type::boolean:
      return source.numerator != 0;
   case logical_type::asset:
   case logical_type::extended_asset: {
      auto result = fc::mutable_variant_object()(field_amount, render_number(source))(field_symbol, source.symbol)(
         field_precision, std::to_string(source.precision));
      if (source.type == logical_type::extended_asset)
         result(field_contract, source.contract);
      return result;
   }
   case logical_type::json:
      return source.container;
   default:
      return source.text;
   }
}

value coerce_literal(value source, const type_descriptor& expected) {
   if (source.null) {
      source.type = expected.logical;
      source.primitive = expected.primitive;
      return source;
   }
   if (expected.kind == type_kind::optional || expected.kind == type_kind::extension)
      return coerce_literal(std::move(source), *expected.element);
   if (source.type != logical_type::text)
      return source;
   try {
      source.primitive = expected.primitive;
      switch (expected.primitive) {
      case primitive_type::name: {
         const auto name = chain::name(source.text);
         source.numerator = name.to_uint64_t();
         source.text = name.to_string();
         break;
      }
      case primitive_type::slug_name: {
         const auto name = fc::slug_name(source.text);
         source.numerator = name.to_uint64_t();
         source.text = name.to_string();
         break;
      }
      case primitive_type::time_point:
      case primitive_type::time_point_sec:
      case primitive_type::block_timestamp_type: {
         const auto microseconds = parse_time(source.text);
         source.numerator = microseconds;
         source.type = logical_type::time;
         source.text = format_time(microseconds);
         break;
      }
      case primitive_type::asset:
         source = from_asset(asset::from_string(source.text));
         break;
      case primitive_type::symbol:
         source.text = symbol::from_string(source.text).to_string();
         break;
      default:
         break;
      }
   } catch (const fc::exception&) {
      throw query_error(error_kind::VALUE_ERROR, "Invalid typed literal");
   } catch (const std::exception&) {
      throw query_error(error_kind::VALUE_ERROR, "Invalid typed literal");
   }
   return source;
}

void compile_schema(table_schema& schema, query_budget& budget) {
   descriptor_compiler compiler(schema.abi, budget);
   schema.row_type = compiler.create(schema.table.type);
   if (schema.row_type->kind != type_kind::structure)
      throw query_error(error_kind::QUERY_SEMANTICS, "Table value must be a struct");
   if (schema.table.key_names.size() != schema.table.key_types.size())
      throw query_error(error_kind::QUERY_SEMANTICS, "Invalid ABI key fields");
   auto key = std::make_shared<type_descriptor>();
   key->kind = type_kind::structure;
   key->logical = logical_type::json;
   for (size_t i = 0; i < schema.table.key_names.size(); ++i)
      key->fields.push_back({schema.table.key_names[i], compiler.create(schema.table.key_types[i])});
   schema.key_type = std::move(key);
   try {
      schema.key_shapes = be_key_codec::build_key_shapes(schema.abi, schema.table.key_names, schema.table.key_types);
   } catch (const fc::exception&) {
      throw query_error(error_kind::QUERY_SEMANTICS, "Unsupported ABI key shape");
   }
}

std::vector<value> decode_fields(const typed_plan& plan, const chain_apis::owned_table_row& row, query_budget& budget) {
   try {
      stream input(row.value.data(), row.value.size());
      const auto decoded = decode_node(*plan.schemas.front()->row_type, input, budget);
      if (input.remaining())
         throw query_error(error_kind::ROW_DECODE_ERROR, "Trailing row bytes");
      be_key_codec::reader key_input(row.key.data(), row.key.size());
      fc::mutable_variant_object key;
      for (size_t i = 0; i < plan.schemas.front()->key_shapes.size(); ++i)
         key(plan.schemas.front()->key_shapes[i].name,
             decode_key_node(key_input, plan.schemas.front()->key_shapes[i],
                             *plan.schemas.front()->key_type->fields[i].type, budget));
      if (key_input.remaining())
         throw query_error(error_kind::ROW_DECODE_ERROR, "Trailing key bytes");
      const fc::variant decoded_key(std::move(key));
      budget.charge_memory(plan.fields.size() * sizeof(value) * node_allocation_factor);
      std::vector<value> result;
      result.reserve(plan.fields.size());
      for (const auto& field : plan.fields) {
         const fc::variant* cell = field.key ? &decoded_key : &decoded;
         for (const auto& component : field.path) {
            if (cell->is_null())
               break;
            cell = &cell->get_object()[component];
         }
         result.push_back(from_cell(*cell, *field.type));
      }
      return result;
   } catch (const query_error&) {
      throw;
   } catch (const fc::exception&) {
      throw query_error(error_kind::ROW_DECODE_ERROR, "Invalid ABI row or key encoding");
   } catch (const std::exception&) {
      throw query_error(error_kind::ROW_DECODE_ERROR, "Invalid ABI row or key encoding");
   }
}
} // namespace sysio::query_engine
