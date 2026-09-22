#include <fc/variant_object.hpp>
#include <sysio/query_engine_plugin/query.hpp>

#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <cstring>
#include <map>
#include <numeric>
#include <tuple>

namespace sysio::query_engine_plugin {
namespace {
constexpr uint64_t allocation_factor = 2;
constexpr uint64_t map_node_overhead = 128;

/// Charge value storage and owned strings before a copy or a vector growth.
void charge_values(const std::vector<value>& values, query_budget& budget) {
   budget.charge_memory(values.size() * sizeof(value) * allocation_factor);
   for (const auto& value : values)
      budget.charge_memory(value.text.size() + value.symbol.size() + value.contract.size());
}

/// Get a bound expression from source fields or finalized aggregate slots.
const value& expression_value(const expression& expression, const std::vector<value>& fields,
                              const std::vector<value>& aggregates) {
   switch (expression.kind) {
   case expression_kind::literal:
      return expression.literal;
   case expression_kind::field:
      return fields.at(*expression.field_slot);
   case expression_kind::aggregate:
      return aggregates.at(*expression.aggregate_slot);
   }
   throw query_error(error_kind::INTERNAL_ERROR, "Invalid bound expression");
}

/// Evaluate SQL predicates with UNKNOWN preserved through all Boolean operators.
truth evaluate_predicate(const predicate& predicate, const std::vector<value>& fields,
                         const std::vector<value>& aggregates, query_budget& budget) {
   budget.check();
   switch (predicate.kind) {
   case predicate_kind::logical_not: {
      const auto child = evaluate_predicate(predicate.children.at(0), fields, aggregates, budget);
      return child == truth::unknown ? child : child == truth::true_value ? truth::false_value : truth::true_value;
   }
   case predicate_kind::logical_and:
   case predicate_kind::logical_or: {
      const bool conjunction = predicate.kind == predicate_kind::logical_and;
      const auto decisive = conjunction ? truth::false_value : truth::true_value;
      bool unknown = false;
      for (const auto& child : predicate.children) {
         const auto result = evaluate_predicate(child, fields, aggregates, budget);
         if (result == decisive)
            return decisive;
         unknown = unknown || result == truth::unknown;
      }
      return unknown ? truth::unknown : conjunction ? truth::true_value : truth::false_value;
   }
   case predicate_kind::is_null: {
      const bool null = expression_value(predicate.left, fields, aggregates).null;
      return null != predicate.negated ? truth::true_value : truth::false_value;
   }
   case predicate_kind::comparison: {
      const auto& left = expression_value(predicate.left, fields, aggregates);
      const auto& right = expression_value(predicate.right, fields, aggregates);
      if (left.null || right.null)
         return truth::unknown;
      const auto comparison = compare_values(left, right);
      bool result = false;
      switch (predicate.operation) {
      case comparison_operator::equal:
         result = comparison == 0;
         break;
      case comparison_operator::not_equal:
         result = comparison != 0;
         break;
      case comparison_operator::less:
         result = comparison < 0;
         break;
      case comparison_operator::less_equal:
         result = comparison <= 0;
         break;
      case comparison_operator::greater:
         result = comparison > 0;
         break;
      case comparison_operator::greater_equal:
         result = comparison >= 0;
         break;
      }
      return result ? truth::true_value : truth::false_value;
   }
   }
   throw query_error(error_kind::INTERNAL_ERROR, "Invalid bound predicate");
}

/// A canonical typed group key orders units before magnitudes and joins all nulls.
struct group_less {
   query_budget* budget;
   bool operator()(const std::vector<value>& left, const std::vector<value>& right) const {
      budget->check();
      for (size_t i = 0; i < left.size(); ++i) {
         const auto& a = left[i];
         const auto& b = right[i];
         if (a.null != b.null)
            return !a.null;
         if (a.null)
            continue;
         if (a.type != b.type)
            return a.type < b.type;
         const auto a_unit = std::tie(a.symbol, a.precision, a.contract);
         const auto b_unit = std::tie(b.symbol, b.precision, b.contract);
         if (a_unit != b_unit)
            return a_unit < b_unit;
         const auto comparison = compare_values(a, b);
         if (comparison)
            return comparison < 0;
      }
      return false;
   }
};

/// Per-group accumulators retain an exact sum and independent non-null count.
struct accumulator {
   value accumulated;
   integer count = 0;
};
struct group_state {
   std::vector<value> fields;
   std::vector<accumulator> accumulators;
};
struct projected_row {
   std::vector<value> cells;
   std::vector<char> primary_key;
   chain::name owner;
   uint64_t group_ordinal = 0;
};

/// Accumulate all input before finalizing AVG or applying HAVING/LIMIT.
void accumulate(group_state& group, const typed_plan& plan, const std::vector<value>& fields, query_budget& budget) {
   for (size_t i = 0; i < plan.aggregates.size(); ++i) {
      budget.check();
      const auto& slot = plan.aggregates[i];
      auto& accumulator = group.accumulators[i];
      const value* source = slot.field ? &fields[*slot.field] : nullptr;
      if (source && source->null)
         continue;
      ++accumulator.count;
      if (slot.function == aggregate_function::COUNT)
         continue;
      if (slot.function == aggregate_function::SUM || slot.function == aggregate_function::AVG)
         add_value(accumulator.accumulated, *source);
      else if (accumulator.accumulated.null)
         accumulator.accumulated = *source;
      else {
         const auto comparison = compare_values(*source, accumulator.accumulated);
         if ((slot.function == aggregate_function::MIN && comparison < 0) ||
             (slot.function == aggregate_function::MAX && comparison > 0))
            accumulator.accumulated = *source;
      }
   }
}

/// Finalized averages are rational, so HAVING and ordering never see rounded decimals.
std::vector<value> finalize(const group_state& group, const typed_plan& plan, query_budget& budget) {
   budget.charge_memory(plan.aggregates.size() * sizeof(value) * allocation_factor);
   std::vector<value> result;
   result.reserve(plan.aggregates.size());
   for (size_t i = 0; i < plan.aggregates.size(); ++i) {
      const auto& slot = plan.aggregates[i];
      const auto& accumulator = group.accumulators[i];
      value output = accumulator.accumulated;
      output.type = slot.type;
      if (slot.function == aggregate_function::COUNT) {
         output.null = false;
         output.numerator = accumulator.count;
      } else if (slot.function == aggregate_function::AVG && !output.null) {
         // Counts are bounded by max_scan_rows, and checked multiplication rejects overflow.
         output.denominator *= accumulator.count;
      }
      result.push_back(std::move(output));
   }
   return result;
}

/// Projection retains exact values until sort and final JSON normalization.
projected_row project(const typed_plan& plan, const std::vector<value>& fields, const std::vector<value>& aggregates,
                      query_budget& budget) {
   budget.charge_memory(sizeof(projected_row) * allocation_factor +
                        plan.columns.size() * sizeof(value) * allocation_factor);
   projected_row result;
   result.cells.reserve(plan.columns.size());
   for (const auto& item : plan.ast.select) {
      const auto& value = expression_value(item.source, fields, aggregates);
      budget.charge_memory(value.text.size() + value.symbol.size() + value.contract.size());
      result.cells.push_back(value);
   }
   return result;
}
} // namespace

int compare_keys(const std::vector<char>& left, const std::vector<char>& right) {
   const auto size = std::min(left.size(), right.size());
   const auto comparison = size ? std::memcmp(left.data(), right.data(), size) : 0;
   return comparison ? comparison : left.size() < right.size() ? -1 : left.size() > right.size() ? 1 : 0;
}

query_result evaluate(const typed_plan& plan, captured_input input, query_budget& budget) {
   try {
      const std::vector<value> no_aggregates;
      std::map<std::vector<value>, group_state, group_less> groups(group_less{&budget});
      std::vector<projected_row> output;
      if (plan.aggregate && plan.groups.empty()) {
         budget.charge_memory(sizeof(group_state) + map_node_overhead + plan.aggregates.size() * sizeof(accumulator));
         groups.emplace(std::vector<value>{}, group_state{{}, std::vector<accumulator>(plan.aggregates.size())});
      }
      uint64_t matched = 0;
      for (auto& row : input.rows) {
         budget.check();
         auto fields = decode_fields(plan, row.row, budget);
         if (plan.ast.where && evaluate_predicate(*plan.ast.where, fields, no_aggregates, budget) != truth::true_value)
            continue;
         ++matched;
         if (!plan.aggregate) {
            auto projected = project(plan, fields, no_aggregates, budget);
            projected.primary_key = std::move(row.row.key);
            projected.owner = row.owner;
            output.push_back(std::move(projected));
            continue;
         }
         budget.charge_memory(plan.groups.size() * sizeof(value) * allocation_factor);
         std::vector<value> key;
         key.reserve(plan.groups.size());
         for (auto slot : plan.groups)
            key.push_back(fields[slot]);
         auto found = groups.find(key);
         if (found == groups.end()) {
            budget.assert_limit(groups.size() + 1, budget.config.max_groups, option::max_groups);
            charge_values(key, budget);
            charge_values(fields, budget);
            budget.charge_memory(sizeof(group_state) + map_node_overhead +
                                 plan.aggregates.size() * sizeof(accumulator));
            group_state group{fields, std::vector<accumulator>(plan.aggregates.size())};
            found = groups.emplace(std::move(key), std::move(group)).first;
         }
         accumulate(found->second, plan, fields, budget);
      }
      uint64_t ordinal = 0;
      for (const auto& [key, group] : groups) {
         auto aggregates = finalize(group, plan, budget);
         if (plan.ast.having &&
             evaluate_predicate(*plan.ast.having, group.fields, aggregates, budget) != truth::true_value)
            continue;
         auto projected = project(plan, group.fields, aggregates, budget);
         projected.group_ordinal = ordinal++;
         output.push_back(std::move(projected));
      }
      std::sort(output.begin(), output.end(), [&](const auto& left, const auto& right) {
         budget.check();
         for (const auto& order : plan.ast.order_by) {
            const auto& a = left.cells[order.slot];
            const auto& b = right.cells[order.slot];
            if (a.null != b.null)
               return !a.null; // NULL is last in both directions.
            if (a.null)
               continue;
            const auto comparison = compare_values(a, b);
            if (comparison)
               return order.descending ? comparison > 0 : comparison < 0;
         }
         if (plan.aggregate)
            return left.group_ordinal < right.group_ordinal;
         if (left.owner != right.owner)
            return left.owner < right.owner;
         return compare_keys(left.primary_key, right.primary_key) < 0;
      });
      const auto offset = std::min<uint64_t>(output.size(), budget.options.offset);
      const auto count = std::min({uint64_t(output.size()) - offset, plan.ast.limit.value_or(output.size()),
                                   budget.options.limit.value_or(output.size())});
      budget.assert_limit(count, budget.config.max_result_rows, option::max_result_rows);
      budget.charge_memory(count * sizeof(fc::variant_object) * allocation_factor +
                           plan.columns.size() * sizeof(output_column));
      std::vector<fc::variant_object> rows;
      rows.reserve(count);
      for (size_t i = 0; i < count; ++i) {
         fc::mutable_variant_object row;
         for (size_t column = 0; column < plan.columns.size(); ++column) {
            budget.charge_memory(plan.columns[column].name.size());
            row(plan.columns[column].name, to_cell(output[offset + i].cells[column], budget));
         }
         rows.emplace_back(std::move(row));
      }
      std::vector<fc::variant_object> columns;
      for (const auto& column : plan.columns) {
         fc::mutable_variant_object descriptor;
         descriptor(response_field::name, column.name)(response_field::logical_type,
                                                       std::string(magic_enum::enum_name(column.type)))(
            response_field::nullable, column.nullable)(response_field::encoding,
                                                       std::string(magic_enum::enum_name(column.representation)));
         descriptor(response_field::abi_type, column.abi_type ? fc::variant(*column.abi_type) : fc::variant());
         columns.emplace_back(std::move(descriptor));
      }
      auto source =
         fc::mutable_variant_object()(response_field::owners, plan.ast.owners)(response_field::table, plan.ast.table);
      auto stats = fc::mutable_variant_object()(response_field::scanned_rows, std::to_string(budget.scanned_rows))(
         response_field::matched_rows, std::to_string(matched))(response_field::groups, std::to_string(groups.size()))(
         response_field::returned_rows, std::to_string(count))(
         response_field::raw_bytes, std::to_string(budget.raw_bytes))(response_field::elapsed_us,
                                                                      std::to_string(budget.elapsed_us()));
      budget.check();
      return {.source = std::move(source),
              .state = input.state.get_object(),
              .columns = std::move(columns),
              .rows = std::move(rows),
              .stats = std::move(stats)};
   } catch (const std::overflow_error&) {
      throw query_error(error_kind::VALUE_ERROR, "Aggregate overflow");
   }
}
} // namespace sysio::query_engine_plugin
