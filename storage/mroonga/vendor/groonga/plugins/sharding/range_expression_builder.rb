module Groonga
  module Sharding
    class RangeExpressionBuilder
      attr_reader :match_columns_expression

      attr_writer :match_columns
      attr_writer :query
      attr_writer :filter

      def initialize(key, target_range)
        @key = key
        @target_range = target_range
        @match_columns_expression = nil
        @match_columns = nil
        @query = nil
        @filter = nil
      end

      def build_all(expression)
        build_condition(expression)
      end

      def build_partial_min(expression)
        expression.append_object(@key, Operator::PUSH, 1)
        expression.append_operator(Operator::GET_VALUE, 1)
        expression.append_constant(@target_range.min, Operator::PUSH, 1)
        if @target_range.min_border == :include
          expression.append_operator(Operator::GREATER_EQUAL, 2)
        else
          expression.append_operator(Operator::GREATER, 2)
        end
        build_condition(expression)
      end

      def build_partial_max(expression)
        expression.append_object(@key, Operator::PUSH, 1)
        expression.append_operator(Operator::GET_VALUE, 1)
        expression.append_constant(@target_range.max, Operator::PUSH, 1)
        if @target_range.max_border == :include
          expression.append_operator(Operator::LESS_EQUAL, 2)
        else
          expression.append_operator(Operator::LESS, 2)
        end
        build_condition(expression)
      end

      def build_partial_min_and_max(expression)
        between = Groonga::Context.instance["between"]
        expression.append_object(between, Operator::PUSH, 1)
        expression.append_object(@key, Operator::PUSH, 1)
        expression.append_operator(Operator::GET_VALUE, 1)
        expression.append_constant(@target_range.min, Operator::PUSH, 1)
        expression.append_constant(@target_range.min_border,
                                   Operator::PUSH, 1)
        expression.append_constant(@target_range.max, Operator::PUSH, 1)
        expression.append_constant(@target_range.max_border,
                                   Operator::PUSH, 1)
        expression.append_operator(Operator::CALL, 5)
        build_condition(expression)
      end

      private
      def build_condition(expression)
        if @query
          is_empty = expression.empty?
          if @match_columns
            table = Context.instance[expression[0].domain]
            @match_columns_expression = Expression.create(table)
            @match_columns_expression.parse(@match_columns)
          end
          flags = Expression::SYNTAX_QUERY |
            Expression::ALLOW_PRAGMA |
            Expression::ALLOW_COLUMN
          expression.parse(@query,
                           default_column: @match_columns_expression,
                           flags: flags)
          expression.append_operator(Operator::AND, 2) unless is_empty
        end

        if @filter
          is_empty = expression.empty?
          expression.parse(@filter)
          expression.append_operator(Operator::AND, 2) unless is_empty
        end
      end
    end
  end
end
