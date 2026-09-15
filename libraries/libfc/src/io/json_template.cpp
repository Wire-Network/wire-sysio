#include <algorithm>
#include <chrono>
#include <fc/exception/exception.hpp>
#include <fc/io/json.hpp>
#include <fc/io/json_template.hpp>
#include <filesystem>
#include <iterator>
#include <system_error>

namespace fc {

namespace {

constexpr std::string_view token_open = "${";
constexpr std::string_view token_escape = "$${";
constexpr char token_close = '}';
constexpr char modifier_separator = ':';

/// Serialize a value exactly as fc::json emits it -- the one encoder every value in a rendered document goes
/// through: literal values and member keys at compile time, assembled strings and non-integer entries at render
/// time (the same encoder as the HTTP API's response bodies).
std::string serialize(const fc::variant& value) {
   return fc::json::to_string(value, fc::time_point::maximum());
}

} // anonymous namespace

bool is_token_name(std::string_view name) {
   // Explicit ASCII ranges rather than std::isalnum, whose character set follows the process locale.
   const auto is_identifier_char = [](char c) {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
   };
   return !name.empty() && std::ranges::all_of(name, is_identifier_char);
}

void for_each_template_part(std::string_view template_text, const std::function<void(std::string_view)>& on_literal,
                            const std::function<void(std::string_view, std::string_view)>& on_token) {
   std::string literal;
   auto flush_literal = [&]() {
      if (literal.empty())
         return;
      on_literal(literal);
      literal.clear();
   };

   std::size_t pos = 0;
   while (pos < template_text.size()) {
      if (template_text.compare(pos, token_escape.size(), token_escape) == 0) {
         literal += token_open; // "$${" escapes a literal "${"
         pos += token_escape.size();
         continue;
      }
      if (template_text.compare(pos, token_open.size(), token_open) != 0) {
         literal += template_text[pos];
         ++pos;
         continue;
      }

      const auto close = template_text.find(token_close, pos + token_open.size());
      FC_ASSERT(close != std::string_view::npos, "template: unterminated '${{' placeholder at offset {}", pos);
      const auto body = template_text.substr(pos + token_open.size(), close - pos - token_open.size());
      const auto sep = body.find(modifier_separator);
      const auto name = sep == std::string_view::npos ? body : body.substr(0, sep);
      const auto modifier = sep == std::string_view::npos ? std::string_view{} : body.substr(sep + 1);
      flush_literal();
      on_token(name, modifier);
      pos = close + 1;
   }
   flush_literal();
}

json_template_values& json_template_values::set_time(fc::time_point time) {
   const auto instant = time.to_system_clock();
   std::string iso;
   format_timestamp_to(std::back_inserter(iso), instant, json_template_timestamp_format::iso8601);
   const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(instant.time_since_epoch()).count();
   set(json_template_default_token::timestamp, fc::variant{iso});
   set(json_template_default_token::iso8601, fc::variant{std::move(iso)});
   return set(json_template_default_token::epoch_millis, fc::variant{int64_t{millis}});
}

json_template json_template::compile(const fc::variant& document) {
   FC_ASSERT(document.is_object(), "json template: the document must be a JSON object");
   json_template compiled;
   compiled.root_ = compiled.compile_node(document);
   compiled.compiled_ = true;
   return compiled;
}

json_template json_template::parse(std::string_view text) {
   return compile(fc::json::from_string(std::string{text}));
}

json_template json_template::from_file(const std::filesystem::path& path) {
   // The non-throwing overload: a filesystem status error surfaces as the FC_ASSERT this function promises,
   // not as a std::filesystem::filesystem_error.
   std::error_code status_error;
   FC_ASSERT(std::filesystem::is_regular_file(path, status_error), "json template: no such template file '{}'",
             path.string());
   return compile(fc::json::from_file(path));
}

json_template::node json_template::compile_node(const fc::variant& value) {
   node n;
   if (value.is_object()) {
      n.type = node::kind::object;
      for (const auto& entry : value.get_object()) {
         // Keys are never tokenized: a key is emitted exactly as written.
         n.member_keys.push_back(serialize(fc::variant{entry.key()}));
         n.member_values.push_back(compile_node(entry.value()));
      }
      return n;
   }
   if (value.is_array()) {
      n.type = node::kind::array;
      for (const auto& element : value.get_array())
         n.elements.push_back(compile_node(element));
      return n;
   }
   if (!value.is_string()) {
      n.verbatim = serialize(value);
      return n;
   }
   std::vector<part> parts;
   bool has_token = false;
   for_each_template_part(
      value.get_string(),
      [&](std::string_view literal) {
         part p;
         p.literal = std::string{literal};
         parts.push_back(std::move(p));
      },
      [&](std::string_view name, std::string_view modifier) {
         FC_ASSERT(is_token_name(name), "json template: invalid token name '{}'", std::string(name));
         part p;
         p.is_token = true;
         p.name = std::string{name};
         if (name == magic_enum::enum_name(json_template_default_token::timestamp)) {
            if (!modifier.empty()) {
               const auto format = magic_enum::enum_cast<json_template_timestamp_format>(modifier);
               FC_ASSERT(format.has_value(), "json template: unknown timestamp modifier '{}'", std::string(modifier));
               // ${timestamp:epoch_millis} reads the epoch_millis entry; ${timestamp:iso8601} is ${timestamp}.
               if (*format == json_template_timestamp_format::epoch_millis)
                  p.name = std::string{magic_enum::enum_name(json_template_default_token::epoch_millis)};
            }
         } else if (name == magic_enum::enum_name(json_template_default_token::level)) {
            if (!modifier.empty()) {
               const auto level_case = magic_enum::enum_cast<json_template_level_case>(modifier);
               FC_ASSERT(level_case.has_value(), "json template: unknown level modifier '{}'", std::string(modifier));
               p.level_case = *level_case;
            }
         } else {
            FC_ASSERT(modifier.empty(), "json template: token '{}' accepts no modifier", std::string(name));
         }
         if (std::ranges::find(tokens_, p.name) == tokens_.end())
            tokens_.push_back(p.name);
         has_token = true;
         parts.push_back(std::move(p));
      });
   if (!has_token) {
      // No placeholder -- but the tokenizer has already turned every `$${` into `${`, so its literal runs are
      // what gets encoded, never the raw template text (which would leave the escape in the document).
      std::string literal;
      for (const auto& p : parts)
         literal += p.literal;
      n.verbatim = serialize(fc::variant{literal});
      return n;
   }
   n.type = node::kind::templated_string;
   n.parts = std::move(parts);
   return n;
}

const fc::variant& json_template::lookup(const part& p, const json_template_values& values) {
   const auto* value = values.find(p.name);
   FC_ASSERT(value != nullptr, "json template: no value supplied for token '{}'", p.name);
   return *value;
}

std::string json_template::token_text(const part& p, const fc::variant& value) {
   if (value.is_string()) {
      // level_case is preserve for every name but level, so this is the identity everywhere else.
      std::string text;
      format_level_name_to(std::back_inserter(text), value.get_string(), p.level_case);
      return text;
   }
   if (value.is_int64())
      return fmt::format("{}", value.as_int64());
   if (value.is_uint64())
      return fmt::format("{}", value.as_uint64());
   return serialize(value); // bool, null, double, object, array: exactly as fc::json emits them
}

std::string json_template::token_json(const part& p, const fc::variant& value) {
   // An integer renders bare digits: fc::json quotes integers above 0xffffffff for the HTTP API, a rule a
   // date-mapped epoch_millis must not inherit. A string is recased (level) and encoded; anything else is
   // fc::json's own text.
   if (value.is_int64() || value.is_uint64())
      return token_text(p, value);
   if (value.is_string())
      return serialize(fc::variant{token_text(p, value)});
   return serialize(value);
}

void json_template::render_node(const node& n, const json_template_values& values, std::string& dest) const {
   switch (n.type) {
   case node::kind::verbatim:
      dest += n.verbatim;
      return;
   case node::kind::object: {
      dest += '{';
      for (std::size_t i = 0; i < n.member_keys.size(); ++i) {
         if (i > 0)
            dest += ',';
         dest += n.member_keys[i];
         dest += ':';
         render_node(n.member_values[i], values, dest);
      }
      dest += '}';
      return;
   }
   case node::kind::array: {
      dest += '[';
      for (std::size_t i = 0; i < n.elements.size(); ++i) {
         if (i > 0)
            dest += ',';
         render_node(n.elements[i], values, dest);
      }
      dest += ']';
      return;
   }
   case node::kind::templated_string: {
      // A lone placeholder yields the entry's typed value; anything else is a JSON string: the placeholders'
      // text is substituted into the literal runs (unescaped text from the parsed template) and the whole
      // value is then encoded by fc::json -- the one encoder every string in the document goes through.
      if (n.parts.size() == 1 && n.parts.front().is_token) {
         dest += token_json(n.parts.front(), lookup(n.parts.front(), values));
         return;
      }
      std::string assembled;
      for (const auto& p : n.parts)
         assembled += p.is_token ? token_text(p, lookup(p, values)) : p.literal;
      dest += serialize(fc::variant{assembled});
      return;
   }
   }
}

std::string json_template::render(const json_template_values& values) const {
   FC_ASSERT(compiled_, "json template: render() called on a template that was never compiled");
   std::string dest;
   render_node(root_, values, dest);
   return dest;
}

bool json_template::contains(std::string_view name) const {
   return std::ranges::any_of(tokens_, [name](const std::string& token) { return token == name; });
}

} // namespace fc
