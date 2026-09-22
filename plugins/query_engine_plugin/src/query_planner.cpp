#include <sysio/query_engine_plugin/query.hpp>

#include <algorithm>
#include <map>
#include <set>

namespace sysio::query_engine_plugin {
namespace {
constexpr uint32_t byte_bits = 8;
constexpr auto path_separator = ".";

/// Canonical default output name preserves all explicitly named path components.
std::string output_name(const field_path& path) {
   std::string result;
   for (const auto& component : path) {
      if (!result.empty())
         result += path_separator;
      result += component;
   }
   return result;
}

/// Strip optional/extension wrappers while recording nullability along a field path.
std::shared_ptr<const type_descriptor> unwrap(std::shared_ptr<const type_descriptor> type, bool& nullable) {
   while (type->kind == type_kind::optional || type->kind == type_kind::extension) {
      nullable = true;
      type = type->element;
   }
   return type;
}

/// Locate aggregate syntax before binding HAVING-only slots.
bool contains_aggregate(const predicate& predicate) {
   if (predicate.left.kind == expression_kind::aggregate || predicate.right.kind == expression_kind::aggregate)
      return true;
   return std::any_of(predicate.children.begin(), predicate.children.end(), contains_aggregate);
}

/// Bind the owned AST exclusively against copied ABI descriptors.
class planner {
public:
   planner(ast_query ast, std::vector<std::shared_ptr<const table_schema>> schemas, query_budget& budget)
      : budget(budget) {
      plan.ast = std::move(ast);
      plan.schemas = std::move(schemas);
      plan.scan.code = plan.schemas.front()->owner;
      plan.scan.table_id = plan.schemas.front()->table.table_id;
      plan.scan.page_rows = constants::page_rows;
   }

   typed_plan create() {
      for (const auto& path : plan.ast.group_by) {
         const auto slot = bind_field(path);
         assert_ordered(plan.fields[slot].type->logical);
         if (std::find(plan.groups.begin(), plan.groups.end(), slot) == plan.groups.end())
            plan.groups.push_back(slot);
      }
      plan.aggregate = !plan.groups.empty() || (plan.ast.having && contains_aggregate(*plan.ast.having)) ||
                       std::any_of(plan.ast.select.begin(), plan.ast.select.end(),
                                   [](const auto& item) { return item.source.kind == expression_kind::aggregate; });
      if (std::any_of(plan.ast.select.begin(), plan.ast.select.end(), [](const auto& item) { return item.star; })) {
         if (plan.ast.select.size() != 1 || plan.aggregate)
            throw query_error(error_kind::QUERY_SEMANTICS, "SELECT * must be the only projection in a detail query");
         plan.ast.select.clear();
         for (const auto& field : plan.schemas.front()->row_type->fields) {
            select_item item;
            item.source.path = {constants::value_namespace, field.name};
            item.alias = field.name;
            plan.ast.select.push_back(std::move(item));
         }
      }
      std::set<std::string> names;
      for (auto& item : plan.ast.select) {
         budget.check();
         bind_expression(item.source, false, true);
         const auto name = item.alias.value_or(output_name(item.source.path));
         if (name.empty() || name == constants::key_namespace || name == constants::value_namespace ||
             !names.insert(name).second)
            throw query_error(error_kind::QUERY_SEMANTICS, "Duplicate or reserved output name", item.source.span);
         output_column column;
         column.name = name;
         column.type = expression_type(item.source);
         if (item.source.kind == expression_kind::aggregate) {
            aliases.emplace(name, item.source);
            column.nullable = item.source.aggregate != aggregate_function::COUNT;
         } else {
            const auto& field = plan.fields.at(*item.source.field_slot);
            column.nullable = field.nullable;
            column.abi_type = field.type->abi_type;
            assert_grouped(*item.source.field_slot, item.source.span);
         }
         column.representation = value_encoding(column.type);
         plan.columns.push_back(std::move(column));
      }
      if (plan.columns.empty())
         throw query_error(error_kind::QUERY_SEMANTICS, "Projection has no columns");
      for (const auto slot : plan.groups)
         if (aliases.contains(plan.fields[slot].path.back()))
            throw query_error(error_kind::QUERY_SEMANTICS, "Aggregate alias collides with grouped field");
      if (plan.ast.where)
         bind_predicate(*plan.ast.where, false);
      if (plan.ast.having) {
         if (!plan.aggregate)
            throw query_error(error_kind::QUERY_SEMANTICS, "HAVING requires an aggregate query");
         bind_predicate(*plan.ast.having, true);
      }
      for (auto& order : plan.ast.order_by) {
         auto found = std::find_if(plan.columns.begin(), plan.columns.end(),
                                   [&](const auto& column) { return column.name == order.name; });
         if (found == plan.columns.end())
            throw query_error(error_kind::QUERY_SEMANTICS, "ORDER BY must name an output column");
         assert_ordered(found->type);
         order.slot = found - plan.columns.begin();
      }
      create_range();
      budget.check();
      return std::move(plan);
   }

private:
   /// Deduplicate field slots by namespace and component path.
   size_t bind_field(field_path path) {
      bool key = false;
      if (path.size() > 1 && (path.front() == constants::key_namespace || path.front() == constants::value_namespace)) {
         key = path.front() == constants::key_namespace;
         path.erase(path.begin());
      }
      for (size_t i = 0; i < plan.fields.size(); ++i)
         if (plan.fields[i].key == key && plan.fields[i].path == path)
            return i;
      auto type = key ? plan.schemas.front()->key_type : plan.schemas.front()->row_type;
      bool nullable = false;
      for (const auto& component : path) {
         type = unwrap(std::move(type), nullable);
         if (type->kind != type_kind::structure)
            throw query_error(error_kind::QUERY_SEMANTICS, "Field path must traverse structs");
         const auto found = std::find_if(type->fields.begin(), type->fields.end(),
                                         [&](const auto& field) { return field.name == component; });
         if (found == type->fields.end())
            throw query_error(error_kind::QUERY_SEMANTICS, "Unknown ABI field");
         type = found->type;
      }
      type = unwrap(std::move(type), nullable);
      budget.charge_memory(sizeof(bound_field) * 2 + path.size() * sizeof(std::string));
      plan.fields.push_back({std::move(path), key, nullable, std::move(type)});
      return plan.fields.size() - 1;
   }

   /// Validate GROUP BY explicitly; primary-key dependencies do not imply grouping.
   void assert_grouped(size_t slot, source_span span) const {
      if (plan.aggregate && std::find(plan.groups.begin(), plan.groups.end(), slot) == plan.groups.end())
         throw query_error(error_kind::QUERY_SEMANTICS, "Source field must appear in GROUP BY", span);
   }

   /// Containers and floats have a projection-only contract.
   static void assert_ordered(logical_type type) {
      if (!is_ordered(type))
         throw query_error(error_kind::QUERY_SEMANTICS, "Container and IEEE values are projection-only");
   }

   /// Resolve fields/aggregates, allowing aggregate aliases only in HAVING.
   void bind_expression(expression& expression, bool having, bool aggregates_allowed) {
      if (expression.kind == expression_kind::literal)
         return;
      if (having && expression.kind == expression_kind::field && expression.path.size() == 1) {
         const auto alias = aliases.find(expression.path.front());
         if (alias != aliases.end()) {
            expression = alias->second;
            return;
         }
      }
      if (expression.kind == expression_kind::field) {
         expression.field_slot = bind_field(expression.path);
         if (having)
            assert_grouped(*expression.field_slot, expression.span);
         return;
      }
      if (!aggregates_allowed)
         throw query_error(error_kind::QUERY_SEMANTICS, "Aggregates are not allowed in WHERE", expression.span);
      if (expression.star && expression.aggregate != aggregate_function::COUNT)
         throw query_error(error_kind::QUERY_SEMANTICS, "Only COUNT accepts *", expression.span);
      aggregate_slot slot;
      slot.function = expression.aggregate;
      if (!expression.star) {
         slot.field = bind_field(expression.path);
         slot.type = plan.fields[*slot.field].type->logical;
         assert_ordered(slot.type);
         if ((slot.function == aggregate_function::SUM || slot.function == aggregate_function::AVG) &&
             !is_numeric(slot.type) && slot.type != logical_type::asset && slot.type != logical_type::extended_asset)
            throw query_error(error_kind::QUERY_SEMANTICS, "SUM and AVG require numeric or asset fields",
                              expression.span);
      }
      if (slot.function == aggregate_function::COUNT)
         slot.type = logical_type::integer;
      else if (slot.function == aggregate_function::AVG && is_numeric(slot.type))
         slot.type = logical_type::decimal;
      for (size_t i = 0; i < plan.aggregates.size(); ++i) {
         if (plan.aggregates[i].function == slot.function && plan.aggregates[i].field == slot.field) {
            expression.aggregate_slot = i;
            return;
         }
      }
      budget.charge_memory(sizeof(aggregate_slot) * 2);
      expression.aggregate_slot = plan.aggregates.size();
      plan.aggregates.push_back(slot);
   }

   logical_type expression_type(const expression& expression) const {
      if (expression.kind == expression_kind::literal)
         return expression.literal.type;
      if (expression.kind == expression_kind::aggregate)
         return plan.aggregates[*expression.aggregate_slot].type;
      return plan.fields[*expression.field_slot].type->logical;
   }

   /// A literal can be coerced by the expected source ABI, including an aggregate's input unit.
   const type_descriptor* expected_type(const expression& expression) const {
      if (expression.field_slot)
         return plan.fields[*expression.field_slot].type.get();
      if (expression.aggregate_slot) {
         const auto& slot = plan.aggregates[*expression.aggregate_slot];
         if (slot.field && slot.function != aggregate_function::COUNT)
            return plan.fields[*slot.field].type.get();
      }
      return nullptr;
   }

   void bind_predicate(predicate& predicate, bool having) {
      budget.check();
      if (!predicate.children.empty()) {
         for (auto& child : predicate.children)
            bind_predicate(child, having);
         return;
      }
      bind_expression(predicate.left, having, having);
      if (predicate.kind == predicate_kind::is_null)
         return;
      bind_expression(predicate.right, having, having);
      if (predicate.left.kind == expression_kind::literal)
         if (const auto* type = expected_type(predicate.right))
            predicate.left.literal = coerce_literal(std::move(predicate.left.literal), *type);
      if (predicate.right.kind == expression_kind::literal)
         if (const auto* type = expected_type(predicate.left))
            predicate.right.literal = coerce_literal(std::move(predicate.right.literal), *type);
      const auto left = expression_type(predicate.left), right = expression_type(predicate.right);
      assert_ordered(left);
      assert_ordered(right);
      if ((predicate.left.kind == expression_kind::literal && predicate.left.literal.null) ||
          (predicate.right.kind == expression_kind::literal && predicate.right.literal.null))
         return;
      if (left != right && !(is_numeric(left) && is_numeric(right)))
         throw query_error(error_kind::QUERY_SEMANTICS, "Incompatible comparison types", predicate.left.span);
      const auto* left_type = expected_type(predicate.left);
      const auto* right_type = expected_type(predicate.right);
      if (left == logical_type::text && left_type && right_type && left_type->primitive != right_type->primitive)
         throw query_error(error_kind::QUERY_SEMANTICS, "Incompatible text families", predicate.left.span);
   }

   /// Only a conjunction of field/literal comparisons proves a range; residual evaluation always remains.
   static bool collect_comparisons(const predicate& predicate,
                                   std::vector<const ::sysio::query_engine_plugin::predicate*>& comparisons) {
      if (predicate.kind == predicate_kind::logical_and) {
         for (const auto& child : predicate.children)
            if (!collect_comparisons(child, comparisons))
               return false;
         return true;
      }
      if (predicate.kind == predicate_kind::logical_or || predicate.kind == predicate_kind::logical_not)
         return false;
      if (predicate.kind == predicate_kind::comparison)
         comparisons.push_back(&predicate);
      return true;
   }

   /// Encode only in-range scalar literals. Out-of-domain comparisons remain residual checks.
   std::optional<std::vector<char>> encode_bound(const chain::be_key_codec::key_shape& shape, const value& value) {
      using chain::be_key_codec::key_leaf_kind;
      if (!shape.is_leaf || value.null || !is_ordered(value.type))
         return std::nullopt;
      fc::variant raw;
      if (is_numeric(value.type)) {
         if (value.denominator != 1)
            return std::nullopt;
         uint32_t bits = 0;
         bool signed_type = false;
         switch (shape.kind) {
         case key_leaf_kind::int8:
            signed_type = true;
            [[fallthrough]];
         case key_leaf_kind::uint8:
            bits = sizeof(uint8_t) * byte_bits;
            break;
         case key_leaf_kind::int16:
            signed_type = true;
            [[fallthrough]];
         case key_leaf_kind::uint16:
            bits = sizeof(uint16_t) * byte_bits;
            break;
         case key_leaf_kind::int32:
            signed_type = true;
            [[fallthrough]];
         case key_leaf_kind::uint32:
            bits = sizeof(uint32_t) * byte_bits;
            break;
         case key_leaf_kind::int64:
            signed_type = true;
            [[fallthrough]];
         case key_leaf_kind::uint64:
            bits = sizeof(uint64_t) * byte_bits;
            break;
         case key_leaf_kind::int128:
            signed_type = true;
            [[fallthrough]];
         case key_leaf_kind::uint128:
            bits = sizeof(fc::uint128) * byte_bits;
            break;
         default:
            return std::nullopt;
         }
         const integer top = integer(1) << (bits - (signed_type ? 1 : 0));
         if (value.numerator < (signed_type ? -top : integer(0)) || value.numerator >= top)
            return std::nullopt;
         raw = value.numerator.convert_to<std::string>();
      } else if (value.type == logical_type::boolean)
         raw = value.numerator != 0;
      else if (value.type == logical_type::text)
         raw = value.text;
      else
         return std::nullopt;
      try {
         chain::be_key_codec::writer writer;
         chain::be_key_codec::encode_field(writer, shape.kind, raw);
         return std::move(writer.buf);
      } catch (const fc::exception&) {
         return std::nullopt;
      }
   }

   /// Intersect encoded bounds. Prefix successors avoid numeric overflow and preserve composite ordering.
   void create_range() {
      std::vector<const predicate*> comparisons;
      if (plan.ast.where && !collect_comparisons(*plan.ast.where, comparisons))
         comparisons.clear();
      std::vector<char> prefix;
      for (size_t i = 0; i < plan.schemas.front()->key_shapes.size(); ++i) {
         const auto& shape = plan.schemas.front()->key_shapes[i];
         std::optional<std::vector<char>> equality;
         std::vector<std::pair<comparison_operator, std::vector<char>>> ranges;
         for (const auto* comparison : comparisons) {
            const expression* field = &comparison->left;
            const expression* literal = &comparison->right;
            auto operation = comparison->operation;
            if (field->kind == expression_kind::literal && literal->kind == expression_kind::field) {
               std::swap(field, literal);
               switch (operation) {
               case comparison_operator::less:
                  operation = comparison_operator::greater;
                  break;
               case comparison_operator::less_equal:
                  operation = comparison_operator::greater_equal;
                  break;
               case comparison_operator::greater:
                  operation = comparison_operator::less;
                  break;
               case comparison_operator::greater_equal:
                  operation = comparison_operator::less_equal;
                  break;
               default:
                  break;
               }
            }
            if (field->kind != expression_kind::field || literal->kind != expression_kind::literal)
               continue;
            const auto& bound = plan.fields[*field->field_slot];
            if (!bound.key || bound.path != field_path{shape.name})
               continue;
            auto encoded = encode_bound(shape, literal->literal);
            if (!encoded)
               continue;
            if (operation == comparison_operator::equal) {
               if (equality && *equality != *encoded)
                  plan.empty_range = true;
               equality = std::move(encoded);
            } else if (operation != comparison_operator::not_equal)
               ranges.emplace_back(operation, std::move(*encoded));
         }
         if (equality) {
            budget.charge_memory(equality->size() + prefix.size());
            prefix.insert(prefix.end(), equality->begin(), equality->end());
            plan.scan.lower = prefix;
            plan.scan.upper = chain_apis::primary_prefix_upper(prefix);
            continue;
         }
         for (auto& [operation, bytes] : ranges) {
            bytes.insert(bytes.begin(), prefix.begin(), prefix.end());
            if (operation == comparison_operator::greater || operation == comparison_operator::less_equal) {
               auto next = chain_apis::primary_prefix_upper(bytes);
               if (!next) {
                  if (operation == comparison_operator::greater)
                     plan.empty_range = true;
                  continue;
               }
               bytes = std::move(*next);
            }
            if (operation == comparison_operator::greater || operation == comparison_operator::greater_equal) {
               if (compare_keys(bytes, plan.scan.lower) > 0)
                  plan.scan.lower = std::move(bytes);
            } else if (!plan.scan.upper || compare_keys(bytes, *plan.scan.upper) < 0)
               plan.scan.upper = std::move(bytes);
         }
         break;
      }
      if (plan.scan.upper && compare_keys(plan.scan.lower, *plan.scan.upper) >= 0)
         plan.empty_range = true;
   }

   typed_plan plan;
   query_budget& budget;
   std::map<std::string, expression> aliases;
};
} // namespace

/// Structural compatibility is checked after resolving aliases, before any owner's rows are captured.
bool compatible_type(const type_descriptor& left, const type_descriptor& right, query_budget& budget) {
   budget.check();
   if (left.kind != right.kind || left.logical != right.logical || left.primitive != right.primitive ||
       left.abi_type != right.abi_type || left.fields.size() != right.fields.size() ||
       left.alternatives.size() != right.alternatives.size() || bool(left.element) != bool(right.element))
      return false;
   if (left.element && !compatible_type(*left.element, *right.element, budget))
      return false;
   for (size_t i = 0; i < left.fields.size(); ++i)
      if (left.fields[i].name != right.fields[i].name ||
          !compatible_type(*left.fields[i].type, *right.fields[i].type, budget))
         return false;
   for (size_t i = 0; i < left.alternatives.size(); ++i)
      if (!compatible_type(*left.alternatives[i], *right.alternatives[i], budget))
         return false;
   return true;
}

typed_plan create_plan(ast_query ast, std::vector<table_schema> schemas, query_budget& budget) {
   if (schemas.empty() || schemas.size() != ast.owners.size())
      throw query_error(error_kind::QUERY_SEMANTICS, "Missing table owner metadata");
   budget.charge_memory(sizeof(typed_plan) +
                        schemas.size() * (sizeof(table_schema) + sizeof(std::shared_ptr<table_schema>)));
   std::vector<std::shared_ptr<const table_schema>> owned;
   owned.reserve(schemas.size());
   for (auto& schema : schemas) {
      compile_schema(schema, budget);
      if (!owned.empty() && (!compatible_type(*owned.front()->row_type, *schema.row_type, budget) ||
                             !compatible_type(*owned.front()->key_type, *schema.key_type, budget)))
         throw query_error(error_kind::QUERY_SEMANTICS, "Selected owners have incompatible table schemas");
      owned.push_back(std::make_shared<const table_schema>(std::move(schema)));
   }
   return planner(std::move(ast), std::move(owned), budget).create();
}
} // namespace sysio::query_engine_plugin
