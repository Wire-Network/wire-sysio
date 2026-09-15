#include <boost/test/unit_test.hpp>
#include <cstdint>
#include <fc/exception/exception.hpp>
#include <fc/filesystem.hpp>
#include <fc/io/json.hpp>
#include <fc/io/json_template.hpp>
#include <fc/log/json_layout.hpp>
#include <fc/time.hpp>
#include <fc/variant.hpp>
#include <fc/variant_object.hpp>
#include <fstream>
#include <spdlog/details/log_msg.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using fc::json_template;
using fc::json_template_values;
using token = fc::json_template_default_token;

namespace {

/// A fixed instant; the tests pin both of its renderings.
const fc::time_point reference_time = fc::time_point::from_iso_string("2026-01-02T03:04:05.123");
constexpr std::string_view reference_iso8601 = "2026-01-02T03:04:05.123000Z";
constexpr int64_t reference_sub_second_millis = 123;
constexpr int64_t millis_per_second = 1000;
constexpr int sample_line = 42;
constexpr std::string_view sample_message = "hello \"world\"";
/// Above 0xffffffff, so fc::json would quote it; a template renders it bare.
constexpr uint64_t wide_value = uint64_t{1} << 40;

/// The epoch millis of reference_time, derived independently of the renderer's arithmetic.
int64_t reference_epoch_millis() {
   return int64_t{fc::time_point_sec{reference_time}.sec_since_epoch()} * millis_per_second +
          reference_sub_second_millis;
}

/// Values under every default name the tests read, each with distinctive content; @p data adds the `data` entry.
json_template_values sample_values(const fc::variant_object* data = nullptr) {
   json_template_values values;
   values.set_time(reference_time)
      .set(token::level, "info")
      .set(token::message, std::string{sample_message})
      .set(token::logger, "sample_logger")
      .set(token::file, "a.cpp")
      .set(token::line, sample_line)
      .set(token::func, "fn")
      .set(token::thread, "worker");
   if (data)
      values.set(token::data, fc::variant{*data});
   return values;
}

fc::variant_object parse_object(const std::string& text) {
   return fc::json::from_string(text).get_object();
}

/// Render @p layout_text through json_layout for a synthetic info record at reference_time.
std::string render_layout(std::string_view layout_text) {
   const spdlog::details::log_msg record{reference_time.to_system_clock(), spdlog::source_loc{}, "sample_logger",
                                         spdlog::level::info, "x"};
   spdlog::memory_buf_t dest;
   fc::log::json_layout::compile(layout_text).render(record, {}, {}, dest);
   return std::string{dest.data(), dest.size()};
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(json_template_tests)

BOOST_AUTO_TEST_CASE(compile_rejects_a_non_object_bad_names_bad_modifiers_and_unterminated_placeholders) try {
   BOOST_CHECK_THROW(json_template::parse("[1]"), fc::exception);
   BOOST_CHECK_THROW(json_template::parse(R"("just a string")"), fc::exception);
   BOOST_CHECK_THROW(json_template::parse(R"({"a":"${}"})"), fc::exception);         // empty name
   BOOST_CHECK_THROW(json_template::parse(R"({"a":"${no space}"})"), fc::exception); // malformed name
   BOOST_CHECK_THROW(json_template::parse(R"({"a":"${timestamp:bogus}"})"), fc::exception);
   BOOST_CHECK_THROW(json_template::parse(R"({"a":"${level:bogus}"})"), fc::exception);
   BOOST_CHECK_THROW(json_template::parse(R"({"a":"${message:upper}"})"), fc::exception); // modifier elsewhere
   BOOST_CHECK_THROW(json_template::parse(R"({"a":"${custom:upper}"})"), fc::exception);
   BOOST_CHECK_THROW(json_template::parse(R"({"a":"${message"})"), fc::exception); // unterminated
   BOOST_CHECK_THROW(json_template::parse(R"({"a":"${message}")"), fc::exception); // not JSON
   // Any identifier-shaped name compiles: which names exist is the producer's decision, checked at render.
   BOOST_CHECK_NO_THROW(json_template::parse(R"({"a":"${custom-name_2}"})"));
}
FC_LOG_AND_RETHROW()

// A string value that is exactly one placeholder becomes the entry's typed value: bare digits for an integer
// (${epoch_millis}, ${timestamp:epoch_millis}, ${line}), an object for an object entry, a JSON string otherwise.
BOOST_AUTO_TEST_CASE(whole_value_placeholders_render_typed_values) try {
   const auto tpl = json_template::parse(
      R"({"ms":"${epoch_millis}","ts_ms":"${timestamp:epoch_millis}","iso":"${iso8601}","ts":"${timestamp}",)"
      R"("ts_iso":"${timestamp:iso8601}","lvl":"${level}","up":"${level:upper}","low":"${level:lower}",)"
      R"("msg":"${message}","cat":"${logger}","f":"${file}","l":"${line}","fn":"${func}","t":"${thread}",)"
      R"("d":"${data}","x":"${extra_object}"})");
   const fc::variant_object data =
      fc::mutable_variant_object{}("k", 1)("nested", fc::mutable_variant_object{}("q", "v"));
   auto values = sample_values(&data);
   values.set(token::extra_object, fc::variant{fc::mutable_variant_object{}("env", "local")("app", "nodeop")});
   const std::string line = tpl.render(values);

   // raw text: the numbers are bare, not quoted
   BOOST_CHECK(line.find(R"("ms":)" + std::to_string(reference_epoch_millis()) + ",") != std::string::npos);
   BOOST_CHECK(line.find(R"("l":42,)") != std::string::npos);
   const auto doc = parse_object(line);
   BOOST_CHECK_EQUAL(doc["ms"].as_int64(), reference_epoch_millis());
   BOOST_CHECK_EQUAL(doc["ts_ms"].as_int64(), reference_epoch_millis());
   BOOST_CHECK_EQUAL(doc["iso"].as_string(), std::string{reference_iso8601});
   BOOST_CHECK_EQUAL(doc["ts"].as_string(), std::string{reference_iso8601});
   BOOST_CHECK_EQUAL(doc["ts_iso"].as_string(), std::string{reference_iso8601});
   BOOST_CHECK_EQUAL(doc["lvl"].as_string(), "info");
   BOOST_CHECK_EQUAL(doc["up"].as_string(), "INFO");
   BOOST_CHECK_EQUAL(doc["low"].as_string(), "info");
   BOOST_CHECK_EQUAL(doc["msg"].as_string(), std::string{sample_message});
   BOOST_CHECK_EQUAL(doc["cat"].as_string(), "sample_logger");
   BOOST_CHECK_EQUAL(doc["f"].as_string(), "a.cpp");
   BOOST_CHECK_EQUAL(doc["l"].as_int64(), sample_line);
   BOOST_CHECK_EQUAL(doc["fn"].as_string(), "fn");
   BOOST_CHECK_EQUAL(doc["t"].as_string(), "worker");
   BOOST_CHECK_EQUAL(doc["d"].get_object()["k"].as_int64(), 1);
   BOOST_CHECK_EQUAL(doc["d"].get_object()["nested"].get_object()["q"].as_string(), "v");
   BOOST_CHECK_EQUAL(doc["x"].get_object()["env"].as_string(), "local");
   BOOST_CHECK_EQUAL(doc["x"].get_object().size(), 2u);
}
FC_LOG_AND_RETHROW()

// A string that mixes placeholders with text renders as a string with each placeholder substituted -- a string
// entry's text, an integer's digits, anything else's JSON -- and the whole value JSON-escaped; `$${` is a
// literal `${`.
BOOST_AUTO_TEST_CASE(mixed_strings_substitute_placeholder_text_and_escape) try {
   const auto tpl = json_template::parse(R"({"loc":"${file}:${line} ${func}","lit":"$${message} stays",)"
                                         R"("q":"say ${message}","f":"flag ${flag}",)"
                                         R"("both":"$${a} ${message}"})");
   auto values = sample_values();
   values.set("flag", true);
   const auto doc = parse_object(tpl.render(values));
   BOOST_CHECK_EQUAL(doc["loc"].as_string(), "a.cpp:42 fn");
   BOOST_CHECK_EQUAL(doc["lit"].as_string(), "${message} stays");
   BOOST_CHECK_EQUAL(doc["q"].as_string(), "say " + std::string{sample_message});
   BOOST_CHECK_EQUAL(doc["f"].as_string(), "flag true");
   // One string carrying both forms: the escape emits a literal `${a}` and the placeholder after it still
   // substitutes -- an escape does not disarm the rest of the string.
   BOOST_CHECK_EQUAL(doc["both"].as_string(), "${a} " + std::string{sample_message});
}
FC_LOG_AND_RETHROW()

// Every string in a rendered document goes through fc::json's encoder, wherever the value sits: a whole-value
// placeholder and a mixed string escape control characters and prune invalid UTF-8 identically.
BOOST_AUTO_TEST_CASE(control_characters_and_invalid_utf8_encode_the_same_in_every_position) try {
   // \x01 and \x08 escape as \u00xx, \n and " take short escapes, and 0xC3 opens a two-byte UTF-8 sequence that
   // 0x28 does not continue -- fc::json prunes the lead byte.
   const std::string tricky = std::string{"\x01\x08\n\"mid"} + '\xC3' + '\x28' + "tail";
   auto values = sample_values();
   values.set(token::message, tricky);
   const auto tpl = json_template::parse(R"({"whole":"${message}","mixed":"x ${message} y"})");

   // What fc::json itself makes of the value: the text every position must carry.
   const std::string encoded = fc::json::to_string(fc::variant{tricky}, fc::time_point::maximum());
   const std::string decoded = fc::json::from_string(encoded).as_string();
   BOOST_CHECK(decoded != tricky); // the value really does exercise the encoder

   const auto doc = parse_object(tpl.render(values)); // parses, so the rendered document is valid JSON
   BOOST_CHECK_EQUAL(doc["whole"].as_string(), decoded);
   BOOST_CHECK_EQUAL(doc["mixed"].as_string(), "x " + decoded + " y");
}
FC_LOG_AND_RETHROW()

// Everything that is not a placeholder is emitted as written -- nested objects, arrays, numbers, booleans,
// null, and plain strings -- and no particular field is required: a template without tokens is valid.
BOOST_AUTO_TEST_CASE(literal_values_pass_through_verbatim_and_no_field_is_required) try {
   const auto tpl = json_template::parse(R"({"env":"local","n":5,"b":true,"z":null,)"
                                         R"("o":{"k":"${message}","deep":{"list":[1,2]}},)"
                                         R"("a":[1,"${line}",{"q":"x"}]})");
   const auto doc = parse_object(tpl.render(sample_values()));
   BOOST_CHECK_EQUAL(doc["env"].as_string(), "local");
   BOOST_CHECK_EQUAL(doc["n"].as_int64(), 5);
   BOOST_CHECK(doc["b"].as_bool());
   BOOST_CHECK(doc["z"].is_null());
   BOOST_CHECK_EQUAL(doc["o"].get_object()["k"].as_string(), std::string{sample_message});
   BOOST_CHECK_EQUAL(doc["o"].get_object()["deep"].get_object()["list"].get_array().size(), 2u);
   const auto& list = doc["a"].get_array();
   BOOST_REQUIRE_EQUAL(list.size(), 3u);
   BOOST_CHECK_EQUAL(list[1].as_int64(), sample_line);
   BOOST_CHECK_EQUAL(list[2].get_object()["q"].as_string(), "x");
   BOOST_CHECK_EQUAL(json_template::parse(R"({"only":"literal"})").render(sample_values()), R"({"only":"literal"})");
   // Keys are never tokenized -- neither a placeholder nor the escape means anything in a key.
   BOOST_CHECK_EQUAL(json_template::parse(R"({"${message}":"v","$${k}":"w"})").render(sample_values()),
                     R"({"${message}":"v","$${k}":"w"})");
}
FC_LOG_AND_RETHROW()

// Names beyond the defaults are supplied by string and render by the same type rules -- including a wide integer
// as bare digits where fc::json would quote it.
BOOST_AUTO_TEST_CASE(additional_names_render_like_the_defaults) try {
   const auto tpl = json_template::parse(R"({"x":"${custom}","y":"${wide}","z":"${obj}","w":"${wide} wide"})");
   auto values = sample_values();
   values.set("custom", "v").set("wide", wide_value).set("obj", fc::variant{fc::mutable_variant_object{}("k", 1)});
   const std::string line = tpl.render(values);
   BOOST_CHECK(line.find(R"("y":)" + std::to_string(wide_value) + ",") != std::string::npos);
   const auto doc = parse_object(line);
   BOOST_CHECK_EQUAL(doc["x"].as_string(), "v");
   BOOST_CHECK_EQUAL(doc["y"].as_uint64(), wide_value);
   BOOST_CHECK_EQUAL(doc["z"].get_object()["k"].as_int64(), 1);
   BOOST_CHECK_EQUAL(doc["w"].as_string(), std::to_string(wide_value) + " wide");
}
FC_LOG_AND_RETHROW()

// A placeholder whose name has no entry fails at render, not at compile: the template compiles for any producer,
// and tokens() is how a producer checks the names it must supply.
BOOST_AUTO_TEST_CASE(render_asserts_on_a_name_without_a_value) try {
   const auto tpl = json_template::parse(R"({"a":"${message}","b":"${nope}"})");
   BOOST_CHECK(tpl.contains("nope"));
   BOOST_CHECK_THROW(tpl.render(sample_values()), fc::exception);
   auto values = sample_values();
   values.set("nope", "now supplied");
   BOOST_CHECK_EQUAL(parse_object(tpl.render(values))["b"].as_string(), "now supplied");
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(contains_and_tokens_report_resolved_names_in_first_seen_order) try {
   const auto tpl =
      json_template::parse(R"({"a":"${message}","b":"${timestamp:epoch_millis} ${message}","c":{"d":"${data}"}})");
   BOOST_CHECK(tpl.contains(token::message));
   BOOST_CHECK(tpl.contains("epoch_millis")); // what ${timestamp:epoch_millis} reads
   BOOST_CHECK(tpl.contains(token::data));
   BOOST_CHECK(!tpl.contains(token::timestamp));
   const std::vector<std::string> expected{"message", "epoch_millis", "data"};
   BOOST_CHECK(tpl.tokens() == expected);
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_CASE(from_file_reads_and_compiles) try {
   fc::temp_directory dir;
   const auto path = dir.path() / "template.json";
   {
      std::ofstream out{path};
      out << R"({"msg":"${message}"})";
   }
   BOOST_CHECK_EQUAL(parse_object(json_template::from_file(path).render(sample_values()))["msg"].as_string(),
                     std::string{sample_message});
   BOOST_CHECK_THROW(json_template::from_file(dir.path() / "missing.json"), fc::exception);
}
FC_LOG_AND_RETHROW()

// A default-constructed template holds no document: render() says so instead of emitting an empty string. A
// compiled template stays renderable through a copy, which is how a producer hands one to its pipeline.
BOOST_AUTO_TEST_CASE(render_asserts_on_a_template_that_was_never_compiled) try {
   BOOST_CHECK_THROW(json_template{}.render(sample_values()), fc::assert_exception);
   const json_template compiled = json_template::parse(R"({"msg":"${message}"})");
   const json_template copy = compiled;
   BOOST_CHECK_EQUAL(parse_object(copy.render(sample_values()))["msg"].as_string(), std::string{sample_message});
}
FC_LOG_AND_RETHROW()

// json_template_values::object() is the entry set the setters wrote, under the same names find() reads back.
BOOST_AUTO_TEST_CASE(values_object_exposes_every_entry_that_was_set) try {
   const json_template_values values = sample_values();
   const auto& object = values.object();
   const std::vector<std::string> expected{"timestamp", "iso8601", "epoch_millis", "level", "message",
                                           "logger",    "file",    "line",         "func",  "thread"};
   BOOST_REQUIRE_EQUAL(object.size(), expected.size());
   for (const auto& name : expected) {
      BOOST_CHECK(object.find(name) != object.end());
      BOOST_CHECK(values.find(name) != nullptr);
   }
   BOOST_CHECK_EQUAL(object["message"].as_string(), std::string{sample_message});
   BOOST_CHECK_EQUAL(object["epoch_millis"].as_int64(), reference_epoch_millis());
   BOOST_CHECK_EQUAL(object["logger"].as_string(), "sample_logger");
   BOOST_CHECK(object.find("absent") == object.end());
   BOOST_CHECK(values.find("absent") == nullptr);
}
FC_LOG_AND_RETHROW()

// to_data: one member per pair, in the order given; a repeated key replaces the earlier member.
BOOST_AUTO_TEST_CASE(to_data_merges_pairs_under_their_keys) try {
   using entry = std::pair<std::string, fc::variant_object>;
   const fc::variant_object none = fc::to_data();
   BOOST_CHECK_EQUAL(none.size(), 0u);
   const fc::variant_object merged = fc::to_data(entry{"sample_logger", fc::mutable_variant_object{}("head", 1)},
                                                 entry{"resource-monitor_2", fc::mutable_variant_object{}("cpu", 2)},
                                                 entry{"sample_logger", fc::mutable_variant_object{}("head", 3)});
   BOOST_CHECK_EQUAL(merged.size(), 2u);
   BOOST_CHECK_EQUAL(merged["sample_logger"].get_object()["head"].as_int64(), 3);
   BOOST_CHECK_EQUAL(merged["resource-monitor_2"].get_object()["cpu"].as_int64(), 2);
}
FC_LOG_AND_RETHROW()

#ifndef NDEBUG
// A to_data key is code-chosen, so a non-identifier is a defect in the producer: debug builds assert on it.
BOOST_AUTO_TEST_CASE(to_data_asserts_on_a_key_that_is_not_an_identifier) try {
   using entry = std::pair<std::string, fc::variant_object>;
   BOOST_CHECK_THROW(fc::to_data(entry{"bad key", fc::mutable_variant_object{}("k", 1)}), fc::assert_exception);
   BOOST_CHECK_THROW(fc::to_data(entry{"", fc::mutable_variant_object{}("k", 1)}), fc::assert_exception);
   BOOST_CHECK_NO_THROW(fc::to_data(entry{"ok-key_2", fc::mutable_variant_object{}("k", 1)}));
}
FC_LOG_AND_RETHROW()
#endif

// The template's timestamps and level names come from the same emitters json_layout uses, so a template
// renders them exactly as a log record would.
BOOST_AUTO_TEST_CASE(timestamps_and_levels_render_exactly_as_json_layout) try {
   const auto doc =
      parse_object(json_template::parse(R"({"iso":"${timestamp}","ms":"${epoch_millis}","lvl":"${level:upper}"})")
                      .render(sample_values()));
   BOOST_CHECK_EQUAL(doc["iso"].as_string(), render_layout("${timestamp}"));
   BOOST_CHECK_EQUAL(std::to_string(doc["ms"].as_int64()), render_layout("${timestamp:epoch_millis}"));
   BOOST_CHECK_EQUAL(doc["lvl"].as_string(), render_layout("${level:upper}"));
}
FC_LOG_AND_RETHROW()

BOOST_AUTO_TEST_SUITE_END()
