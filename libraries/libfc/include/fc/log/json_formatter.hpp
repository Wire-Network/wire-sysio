#pragma once
#include <fc/spdlog.hpp>
#include <fc/log/json_layout.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace fc::log {

/// spdlog formatter that emits one JSON object per log record, terminated by '\n'.
/// The document shape is a compiled json_layout token template: absent/empty
/// `layout_template` renders the historical fc JSONL shape (fc::log::default_layout --
/// ts/lvl/thread/logger/file/line/func/msg, optional extra). Any other template
/// (e.g. fc::log::es_default_layout) reshapes the document freely. Usable on any
/// sink -- attach via sink->set_formatter(std::make_unique<fc::log::json_formatter>(...)).
class json_formatter final : public spdlog::formatter {
public:
   /// @param extra_fields    static key/value pairs. Where they land is the template's
   ///                        choice: ${extra_object} (the historical `,"extra":{...}`
   ///                        object) or ${extra_flat} (flattened at the placement point,
   ///                        e.g. document top level for the es shape). Key uniqueness
   ///                        vs. template-authored keys is the template author's
   ///                        responsibility -- the template controls placement.
   /// @param layout_template json_layout token template; empty compiles
   ///                        fc::log::default_layout (the historical output).
   ///                        FC_ASSERTs on a malformed template (unknown token /
   ///                        modifier / unterminated placeholder).
   explicit json_formatter(std::map<std::string, std::string> extra_fields = {}, std::string layout_template = {});

   void format(const spdlog::details::log_msg& msg, spdlog::memory_buf_t& dest) override;
   std::unique_ptr<spdlog::formatter> clone() const override;

private:
   std::map<std::string, std::string> extra_fields_;
   std::string                        layout_template_;      ///< kept for clone()
   json_layout                        compiled_;             ///< compiled once in the ctor
   std::string                        extra_object_fragment_; ///< pre-escaped `,"extra":{...}` or ""
   std::string                        extra_flat_fragment_;   ///< pre-escaped `,"k":"v",...` or ""
};

} // namespace fc::log
