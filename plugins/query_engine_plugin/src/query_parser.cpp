#include <sysio/query_engine_plugin/query.hpp>

#include <magic_enum/magic_enum.hpp>

#include <cctype>
#include <charconv>
#include <set>

#include <WireQueryLexer.h>
#include <WireQueryParser.h>

namespace sysio::query_engine {
namespace {
/// A throwing listener prevents ANTLR recovery from silently changing the query.
class error_listener final : public antlr4::BaseErrorListener {
   void syntaxError(antlr4::Recognizer*, antlr4::Token*, size_t line, size_t column, const std::string&,
                    std::exception_ptr) override {
      throw query_error(error_kind::QUERY_SYNTAX, "Invalid query syntax",
                        source_span{static_cast<uint32_t>(line), static_cast<uint32_t>(column + 1)});
   }
};

/// Decode SQL doubled-quote escaping; a backslash is always a literal backslash.
std::string unquote(std::string text) {
   const char quote = text.front();
   std::string decoded;
   decoded.reserve(text.size() - 2);
   for (size_t i = 1; i + 1 < text.size(); ++i) {
      decoded.push_back(text[i]);
      if (text[i] == quote)
         ++i;
   }
   return decoded;
}

/// Preserve identifier spelling, removing only SQL quote delimiters.
std::string identifier(WireQueryParser::IdentifierContext* context) {
   auto text = context->getText();
   return context->QUOTED_IDENTIFIER() ? unquote(std::move(text)) : text;
}

/// Owned AST adapter. Node and allocation accounting happens before recursive construction.
class ast_builder {
public:
   explicit ast_builder(query_budget& budget)
      : budget(budget) {}

   /// Convert the already bounded parse tree while its parser/token owners are alive.
   ast_query create(WireQueryParser::QueryContext* root) {
      ast_query result;
      result.table = identifier(root->table);
      if (root->owner) {
         if (root->OWNER())
            throw query_error(error_kind::QUERY_SEMANTICS, "Specify qualified owner or OWNER clause, not both");
         result.owners.push_back(identifier(root->owner));
      }
      for (auto* owner : root->ownerName()) {
         budget.assert_limit(result.owners.size() + 1, constants::max_owners, "query-max-owners");
         node();
         result.owners.push_back(owner->STRING() ? unquote(owner->STRING()->getText())
                                                 : identifier(owner->identifier()));
      }
      if (result.owners.empty())
         throw query_error(error_kind::QUERY_SEMANTICS, "An explicit table owner is required");
      std::set<chain::name> unique;
      for (auto& owner : result.owners) {
         chain::name name;
         try {
            name = chain::name(owner);
         } catch (const fc::exception&) {
            throw query_error(error_kind::QUERY_SEMANTICS, "Invalid owner name");
         }
         if (name.to_string().empty() || !unique.insert(name).second)
            throw query_error(error_kind::QUERY_SEMANTICS, "Duplicate or empty table owner");
         owner = name.to_string();
      }
      result.owners.clear();
      for (const auto& owner : unique)
         result.owners.push_back(owner.to_string());
      for (auto* item : root->selectItem()) {
         node();
         select_item selected;
         selected.star = item->STAR() != nullptr;
         if (item->fieldPath())
            selected.source.path = path(item->fieldPath());
         if (item->aggregateCall())
            selected.source = aggregate(item->aggregateCall());
         if (item->identifier())
            selected.alias = identifier(item->identifier());
         selected.source.span = span(item);
         result.select.push_back(std::move(selected));
      }
      if (root->wherePredicate)
         result.where = disjunction(root->wherePredicate->orPredicate());
      for (auto* group : root->fieldPath())
         result.group_by.push_back(path(group));
      if (root->havingPredicate)
         result.having = disjunction(root->havingPredicate->orPredicate());
      for (auto* order : root->orderItem()) {
         node();
         result.order_by.push_back({identifier(order->identifier()), order->DESC() != nullptr});
      }
      if (root->limit) {
         const auto text = root->limit->getText();
         uint64_t limit = 0;
         auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), limit);
         if (error != std::errc() || end != text.data() + text.size())
            throw query_error(error_kind::QUERY_SEMANTICS, "LIMIT exceeds unsigned integer range");
         result.limit = limit;
      }
      return result;
   }

private:
   query_budget& budget;
   uint32_t nodes = 0;

   /// Charge a conservative full node including vector growth and temporary copies.
   void node() {
      budget.assert_limit(++nodes, constants::max_ast_nodes, "query-max-ast-nodes");
      budget.charge_memory(2 * sizeof(predicate));
   }
   /// Obtain the start location without retaining a token pointer.
   source_span span(antlr4::ParserRuleContext* context) {
      return {static_cast<uint32_t>(context->getStart()->getLine()),
              static_cast<uint32_t>(context->getStart()->getCharPositionInLine() + 1)};
   }
   /// Copy a path, preserving quoted path components.
   field_path path(WireQueryParser::FieldPathContext* context) {
      node();
      field_path result;
      for (auto* part : context->identifier())
         result.push_back(identifier(part));
      return result;
   }
   /// Bind only syntax here; COUNT(*) and other aggregate semantics belong to the planner.
   expression aggregate(WireQueryParser::AggregateCallContext* context) {
      node();
      expression result;
      result.kind = expression_kind::aggregate;
      result.span = span(context);
      auto name = context->aggregate()->getText();
      for (auto& c : name)
         c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      result.aggregate = magic_enum::enum_cast<aggregate_function>(name).value();
      result.star = context->STAR() != nullptr;
      if (context->fieldPath())
         result.path = path(context->fieldPath());
      return result;
   }
   /// Convert numeric token text without passing through a floating-point parser.
   expression scalar(WireQueryParser::ExpressionContext* context) {
      node();
      if (context->aggregateCall())
         return aggregate(context->aggregateCall());
      expression result;
      result.span = span(context);
      if (context->fieldPath()) {
         result.path = path(context->fieldPath());
         return result;
      }
      result.kind = expression_kind::literal;
      auto* literal = context->literal();
      result.literal.null = literal->NULL_LITERAL() != nullptr;
      if (literal->STRING()) {
         result.literal.text = unquote(literal->getText());
      } else if (literal->TRUE_LITERAL() || literal->FALSE_LITERAL()) {
         result.literal.type = logical_type::boolean;
         result.literal.numerator = literal->TRUE_LITERAL() ? 1 : 0;
      } else if (!literal->NULL_LITERAL()) {
         result.literal = parse_number(literal->getText());
      }
      return result;
   }
   /// OR and AND are n-ary nodes, so long flat predicates do not cause deep recursion.
   predicate disjunction(WireQueryParser::OrPredicateContext* context) {
      node();
      if (context->andPredicate().size() == 1)
         return conjunction(context->andPredicate(0));
      predicate result;
      result.kind = predicate_kind::logical_or;
      for (auto* child : context->andPredicate())
         result.children.push_back(conjunction(child));
      return result;
   }
   /// Bind conjunction children in source order, preserving three-valued short circuiting.
   predicate conjunction(WireQueryParser::AndPredicateContext* context) {
      node();
      if (context->notPredicate().size() == 1)
         return negation(context->notPredicate(0));
      predicate result;
      result.kind = predicate_kind::logical_and;
      for (auto* child : context->notPredicate())
         result.children.push_back(negation(child));
      return result;
   }
   /// NOT binds more loosely than comparison/IS NULL and more tightly than AND.
   predicate negation(WireQueryParser::NotPredicateContext* context) {
      node();
      predicate result;
      if (context->NOT()) {
         result.kind = predicate_kind::logical_not;
         result.children.push_back(negation(context->notPredicate()));
         return result;
      }
      auto* atom = context->predicateAtom();
      if (atom->predicate())
         return disjunction(atom->predicate()->orPredicate());
      result.left = scalar(atom->expression(0));
      if (atom->IS()) {
         result.kind = predicate_kind::is_null;
         result.negated = atom->NOT() != nullptr;
      } else {
         result.right = scalar(atom->expression(1));
         const auto* operation = atom->comparison()->getStart();
         switch (operation->getType()) {
         case WireQueryLexer::EQ:
            result.operation = comparison_operator::equal;
            break;
         case WireQueryLexer::NE:
            result.operation = comparison_operator::not_equal;
            break;
         case WireQueryLexer::LT:
            result.operation = comparison_operator::less;
            break;
         case WireQueryLexer::LE:
            result.operation = comparison_operator::less_equal;
            break;
         case WireQueryLexer::GT:
            result.operation = comparison_operator::greater;
            break;
         case WireQueryLexer::GE:
            result.operation = comparison_operator::greater_equal;
            break;
         default:
            throw query_error(error_kind::QUERY_SYNTAX, "Invalid comparison");
         }
      }
      return result;
   }
};
} // namespace

ast_query parse_query(std::string_view sql, query_budget& budget) {
   budget.assert_limit(sql.size(), budget.config.max_query_bytes, option::max_query_bytes);
   // Covers owned token text/path strings and the bounded parse tree; ANTLR's runtime
   // allocations have an independent byte/token/depth cap and die before chain capture.
   constexpr uint64_t syntax_bytes_multiplier = 64;
   budget.charge_memory(sql.size() * syntax_bytes_multiplier);
   error_listener errors;
   antlr4::ANTLRInputStream input{std::string(sql)};
   WireQueryLexer lexer(&input);
   lexer.removeErrorListeners();
   lexer.addErrorListener(&errors);
   antlr4::CommonTokenStream tokens(&lexer);
   // Fetch incrementally: reject long token streams before asking the parser to allocate a tree.
   uint32_t nesting = 0;
   uint32_t unary = 0;
   std::vector<uint32_t> parentheses;
   parentheses.reserve(constants::max_depth);
   for (uint32_t count = 0;; ++count) {
      budget.assert_limit(count, constants::max_tokens, "query-max-tokens");
      const auto type = tokens.LT(1)->getType();
      if (type == antlr4::Token::EOF)
         break;
      if (type == WireQueryLexer::NOT) {
         ++unary;
      } else if (type == WireQueryLexer::LPAREN) {
         budget.assert_limit(nesting + unary + 1, constants::max_depth, "query-max-depth");
         parentheses.push_back(unary + 1);
         nesting += unary + 1;
         unary = 0;
      } else {
         if (type == WireQueryLexer::RPAREN && !parentheses.empty()) {
            nesting -= parentheses.back();
            parentheses.pop_back();
         }
         unary = 0;
      }
      budget.assert_limit(nesting + unary, constants::max_depth, "query-max-depth");
      tokens.consume();
   }
   tokens.seek(0);
   WireQueryParser parser(&tokens);
   parser.removeErrorListeners();
   parser.addErrorListener(&errors);
   auto* tree = parser.query();
   budget.check();
   return ast_builder(budget).create(tree);
}

} // namespace sysio::query_engine
