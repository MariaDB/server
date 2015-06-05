module Groonga
  module Sharding
    class RangeExpressionBuilder
      def initialize(key, target_range, filter)
        @key = key
        @target_range = target_range
        @filter = filter
      end

      def build_all(expression)
        return if @filter.nil?

        expression.parse(@filter)
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
        if @filter
          expression.parse(@filter)
          expression.append_operator(Operator::AND, 2)
        end
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
        if @filter
          expression.parse(@filter)
          expression.append_operator(Operator::AND, 2)
        end
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
        if @filter
          expression.parse(@filter)
          expression.append_operator(Operator::AND, 2)
        end
      end
    end
  end
end
