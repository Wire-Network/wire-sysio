#pragma once
#include <sysio/query_engine_plugin/query.hpp>

namespace sysio::query_engine_plugin {
/// In-process query API. Results own their rows and metadata; failures throw query_error.
class query_service {
public:
   virtual ~query_service() = default;
   /// Evaluate SQL completely before applying pagination. Absent options use query_options defaults.
   virtual query_result execute(const std::string& query,
                                const std::optional<query_options>& options = std::nullopt) = 0;
};
} // namespace sysio::query_engine_plugin
