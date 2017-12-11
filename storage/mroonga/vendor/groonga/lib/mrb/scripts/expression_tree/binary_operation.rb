module Groonga
  module ExpressionTree
    class BinaryOperation
      attr_reader :operator
      attr_reader :left
      attr_reader :right
      def initialize(operator, left, right)
        @operator = operator
        @left = left
        @right = right
      end

      def build(expression)
        @left.build(expression)
        @right.build(expression)
        expression.append_operator(@operator, 2)
      end

      RANGE_OPERATORS = [
        Operator::LESS,
        Operator::GREATER,
        Operator::LESS_EQUAL,
        Operator::GREATER_EQUAL,
      ]
      def estimate_size(table)
        case @operator
        when *RANGE_OPERATORS
          estimate_size_range(table)
        else
          table.size
        end
      end

      private
      def estimate_size_range(table)
        return table.size unless @left.is_a?(Variable)
        return table.size unless @right.is_a?(Constant)

        column = @left.column
        value = @right.value
        index_info = column.find_index(@operator)
        return table.size if index_info.nil?

        index_column = index_info.index
        lexicon = index_column.lexicon
        options = {}
        case @operator
        when Operator::LESS
          options[:max] = value
          options[:flags] = TableCursorFlags::LT
        when Operator::LESS_EQUAL
          options[:max] = value
          options[:flags] = TableCursorFlags::LE
        when Operator::GREATER
          options[:min] = value
          options[:flags] = TableCursorFlags::GT
        when Operator::GREATER_EQUAL
          options[:min] = value
          options[:flags] = TableCursorFlags::GE
        end
        TableCursor.open(lexicon, options) do |cursor|
          index_column.estimate_size(:lexicon_cursor => cursor)
        end
      end
    end
  end
end
