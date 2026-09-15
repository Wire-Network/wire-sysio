#pragma once

#include <array>
#include <cctype>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <ctime>
#include <fc/exception/exception.hpp>
#include <fc/time.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <filesystem>
#include <fmt/format.h>
#include <functional>
#include <iterator>
#include <magic_enum/magic_enum.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fc {

/// `${timestamp}` renderings, shared by every `${token[:modifier]}` template (fc::log::json_layout, json_template).
enum class json_template_timestamp_format : uint8_t { iso8601, epoch_millis };

/// `${level}` case choices; `preserve` keeps the spelling supplied.
enum class json_template_level_case : uint8_t { preserve, upper, lower };

/// True for the identifier form a template token name and a to_data key must take: ASCII letters, digits, '_' and
/// '-', at least one character.
bool is_token_name(std::string_view name);

/// Walk a `${token[:modifier]}` template text: @p on_literal receives each literal run (a `$${` escape yields a
/// literal `${`), @p on_token receives each placeholder's name and its modifier (empty when none). FC_ASSERTs on
/// an unterminated `${`. Shared by fc::log::json_layout (text layouts) and json_template (JSON-file templates).
void for_each_template_part(std::string_view template_text, const std::function<void(std::string_view)>& on_literal,
                            const std::function<void(std::string_view, std::string_view)>& on_token);

/// Write @p time to @p out in @p format: a bare epoch-millisecond number, or `YYYY-MM-DDTHH:MM:SS.uuuuuuZ` (UTC).
/// Header-only over an output iterator, so a log layout appends to its spdlog buffer and a document template to a
/// std::string with no copy in between.
template <std::output_iterator<char> OutputIt>
OutputIt format_timestamp_to(OutputIt out, std::chrono::system_clock::time_point time,
                             json_template_timestamp_format format) {
   if (format == json_template_timestamp_format::epoch_millis) {
      return fmt::format_to(out, "{}",
                            std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count());
   }
   const auto secs = std::chrono::time_point_cast<std::chrono::seconds>(time);
   const auto us = std::chrono::duration_cast<std::chrono::microseconds>(time - secs).count();
   const std::tm tm_utc = fc::to_utc_tm(std::chrono::system_clock::to_time_t(secs));
   return fmt::format_to(out, "{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:06d}Z", tm_utc.tm_year + 1900,
                         tm_utc.tm_mon + 1, tm_utc.tm_mday, tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec, us);
}

/// Write @p level_name to @p out in @p level_case (level names are ASCII, so no escaping is needed).
template <std::output_iterator<char> OutputIt>
OutputIt format_level_name_to(OutputIt out, std::string_view level_name, json_template_level_case level_case) {
   for (const char c : level_name) {
      const auto uc = static_cast<unsigned char>(c);
      *out++ = level_case == json_template_level_case::upper   ? static_cast<char>(std::toupper(uc))
               : level_case == json_template_level_case::lower ? static_cast<char>(std::tolower(uc))
                                                               : c;
   }
   return out;
}

/// Token names a producer may supply by enumerator: the logging layout's value tokens under the same spelling
/// (fc::log::json_layout_token -- json_layout.hpp asserts the coverage), the two timestamp renderings as standalone
/// names, and `data`. They are defaults, not requirements: a producer sets the ones it has and adds any further name
/// by string through json_template_values.
enum class json_template_default_token : uint8_t {
   timestamp,    ///< `YYYY-MM-DDTHH:MM:SS.uuuuuuZ` (set_time); `${timestamp:epoch_millis}` reads epoch_millis instead
   epoch_millis, ///< the same instant as a bare epoch-millisecond number (set_time)
   iso8601,      ///< the same instant as `YYYY-MM-DDTHH:MM:SS.uuuuuuZ` (set_time)
   level,        ///< a level name; `${level:upper}` / `${level:lower}` recase it at render
   message,      ///< the record's message text
   logger,       ///< the name of the logger or producer the record came from
   file,         ///< source filename, empty when unknown
   line,         ///< source line; an integer entry renders as bare digits
   func,         ///< source function name, empty when unknown
   thread,       ///< the thread name (fc::get_thread_name() in a log record)
   extra_object, ///< the logging sink's extra fields as an object; a producer without them leaves it unset
   data,         ///< a producer's payload object, built with to_data()
};

/// The values a json_template renders from: one entry per token name in a variant object. Any name may be set --
/// a default token by enumerator, anything else by string -- and nothing is required: a template renders the names
/// it references, and json_template::render FC_ASSERTs on a referenced name with no entry, so a producer validates
/// its template against the names it supplies at configuration time (json_template::tokens()).
class json_template_values {
public:
   /// Set (or replace) the entry for @p name.
   json_template_values& set(std::string name, fc::variant value) {
      values_.set(std::move(name), std::move(value));
      return *this;
   }
   /// Set (or replace) a default token's entry.
   json_template_values& set(json_template_default_token token, fc::variant value) {
      return set(std::string{magic_enum::enum_name(token)}, std::move(value));
   }
   /// Set the three timestamp entries from one instant: `timestamp` and `iso8601` as `YYYY-MM-DDTHH:MM:SS.uuuuuuZ`,
   /// `epoch_millis` as a number -- the renderings fc::log::json_layout gives a log record's time.
   json_template_values& set_time(fc::time_point time);
   /// The entry for @p name, or nullptr.
   const fc::variant* find(const std::string& name) const {
      const auto itr = values_.find(name);
      return itr == values_.end() ? nullptr : &itr->value();
   }
   /// Every entry, in insertion order.
   const fc::mutable_variant_object& object() const noexcept { return values_; }

private:
   fc::mutable_variant_object values_;
};

/// Build the object the `data` entry holds from `(key, object)` pairs: each pair becomes one member, so every
/// producer nests its payload under its own key (one per producer) and more can be added later. Keys are
/// code-chosen identifiers, held to is_token_name by a debug-build assert; a repeated key replaces the earlier
/// member.
template <std::same_as<std::pair<std::string, fc::variant_object>>... Pairs>
fc::variant_object to_data(const Pairs&... pairs) {
   const std::array<const std::pair<std::string, fc::variant_object>*, sizeof...(Pairs)> entries{&pairs...};
#ifndef NDEBUG
   // Debug builds only: a key is code-chosen, so a bad one is a defect in the producer, not bad input.
   for (const auto* entry : entries) {
      const std::string& key = entry->first;
      FC_ASSERT(is_token_name(key), "to_data: key '{}' is not an identifier", key);
   }
#endif
   fc::mutable_variant_object mvo;
   for (const auto* entry : entries)
      mvo.set(entry->first, fc::variant{entry->second});
   return mvo;
}

/// A JSON document template: a JSON OBJECT (parsed once from text or a file) whose string values may contain
/// `${name[:modifier]}` placeholders. A name is any run of identifier characters -- the producer, not the template,
/// decides which names exist (json_template_default_token lists the conventional ones). Every other value --
/// literal strings, numbers, booleans, null, nested objects and arrays -- is emitted verbatim, which is how a
/// producer's fixed fields (env, app, principal, ...) are written straight into the template. Nothing else is
/// inspected: no field is required, none is added, and every member the template carries is emitted (a JSON file
/// has no comment syntax, so a "_comment" member would land in every document).
///
/// Rendering emits one line of JSON text, looking each placeholder up by name in the json_template_values. A
/// string value that is exactly one placeholder renders the entry's typed value: an integer as bare digits (never
/// through fc::json, which quotes integers above 0xffffffff for the HTTP API), a string as a JSON string, and an
/// object, array, bool, null, or double exactly as fc::json emits it. A string that mixes placeholders with other
/// text is assembled (a string entry contributes its text, an integer its digits, anything else its JSON) and then
/// encoded as a JSON string. Every string in the document -- literal or assembled -- goes through fc::json's
/// encoder, so control characters and invalid UTF-8 are handled identically everywhere (never through the
/// logging layout's lighter escaper). In a string value `$${` is a literal `${`, whether or not the value also
/// carries a placeholder; object keys are never tokenized and are emitted exactly as written. `timestamp` and
/// `level` are the only names that take a modifier: `${timestamp:epoch_millis}` reads the `epoch_millis` entry
/// (`${timestamp}` and `${timestamp:iso8601}` read `timestamp`), `${level:upper}` / `${level:lower}` recase the
/// `level` entry. Timestamps and level names come from the same emitters fc::log::json_layout uses, so they render
/// exactly as in a log record.
class json_template {
public:
   /// A default-constructed template holds no document: it must be replaced by a compile(), parse(), or
   /// from_file() result before use -- render() FC_ASSERTs on one that was never compiled.
   json_template() = default;

   /// Compile @p document, which must be a JSON object. FC_ASSERTs on a non-object, an empty or malformed token
   /// name, an unknown modifier, a modifier on a name other than timestamp or level, or an unterminated `${`.
   static json_template compile(const fc::variant& document);
   /// fc::json::from_string(@p text), then compile().
   static json_template parse(std::string_view text);
   /// fc::json::from_file(@p path), then compile(); a missing or unparseable file throws fc::exception.
   static json_template from_file(const std::filesystem::path& path);

   /// Render one document from @p values (no trailing newline). FC_ASSERTs on a template that was never compiled
   /// and on a referenced name with no entry.
   std::string render(const json_template_values& values) const;
   /// True when the template references @p name (after modifier resolution, so `${timestamp:epoch_millis}`
   /// counts as `epoch_millis`).
   bool contains(std::string_view name) const;
   /// True when the template references @p token's name, by the same resolution as contains(std::string_view).
   bool contains(json_template_default_token token) const { return contains(magic_enum::enum_name(token)); }
   /// The names the template references, in first-seen order, after modifier resolution.
   const std::vector<std::string>& tokens() const noexcept { return tokens_; }

private:
   /// One piece of a string value: literal text, or a placeholder resolved to the entry it reads.
   struct part {
      std::string literal;
      bool is_token = false;
      std::string name;                                                         ///< the entry read at render
      json_template_level_case level_case = json_template_level_case::preserve; ///< level only
   };
   /// One compiled JSON value.
   struct node {
      enum class kind : uint8_t { verbatim, templated_string, object, array };
      kind type = kind::verbatim;
      std::string verbatim;                 ///< kind::verbatim: the value pre-serialized by fc::json
      std::vector<part> parts;              ///< kind::templated_string
      std::vector<std::string> member_keys; ///< kind::object: pre-serialized key text, in template order
      std::vector<node> member_values;      ///< kind::object: one per key
      std::vector<node> elements;           ///< kind::array
   };

   node compile_node(const fc::variant& value);
   void render_node(const node& n, const json_template_values& values, std::string& dest) const;
   /// The entry @p p reads; FC_ASSERTs when @p values has none.
   static const fc::variant& lookup(const part& p, const json_template_values& values);
   /// The text a placeholder contributes inside a mixed string: a string entry's text (recased for level), an
   /// integer's digits, anything else's JSON.
   static std::string token_text(const part& p, const fc::variant& value);
   /// The JSON a lone placeholder renders: bare digits for an integer, a JSON string for a string (recased for
   /// level), fc::json's text for anything else.
   static std::string token_json(const part& p, const fc::variant& value);

   node root_;
   std::vector<std::string> tokens_;
   /// False until compile() has built root_, so render() can refuse a default-constructed template instead of
   /// silently emitting an empty document.
   bool compiled_ = false;
};

} // namespace fc
