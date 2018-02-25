module Groonga
  module ExpressionTree
    class FunctionCall
      attr_reader :procedure
      attr_reader :arguments
      def initialize(procedure, arguments)
        @procedure = procedure
        @arguments = arguments
      end

      def build(expression)
        @procedure.build(expression)
        @arguments.each do |argument|
          argument.build(expression)
        end
        expression.append_operator(Operator::CALL, @arguments.size)
      end

      def estimate_size(table)
        return table.size unless @procedure.name == "between"

        column, min, min_border, max, max_border = @arguments

        if column.is_a?(Groonga::ExpressionTree::IndexColumn)
          index_column = column.object
        else
          index_info = column.column.find_index(Operator::CALL)
          return table.size if index_info.nil?
          index_column = index_info.index
        end

        while index_column.is_a?(Groonga::Accessor)
          if index_column.have_next?
            index_column = index_column.next
          else
            index_column = index_column.object
            index_info = index_column.find_index(Operator::CALL)
            return table.size if index_info.nil?
            index_column = index_info.index
          end
        end

        lexicon = index_column.lexicon
        options = {
          :min => min.value,
          :max => max.value,
          :flags => 0,
        }
        if min_border.value == "include"
          options[:flags] |= TableCursorFlags::LT
        else
          options[:flags] |= TableCursorFlags::LE
        end
        if max_border.value == "include"
          options[:flags] |= TableCursorFlags::GT
        else
          options[:flags] |= TableCursorFlags::GE
        end

        TableCursor.open(lexicon, options) do |cursor|
          index_column.estimate_size(:lexicon_cursor => cursor)
        end
      end
    end
  end
end
