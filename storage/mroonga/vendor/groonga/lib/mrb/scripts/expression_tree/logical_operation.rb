module Groonga
  module ExpressionTree
    class LogicalOperation
      attr_reader :operator
      attr_reader :nodes
      def initialize(operator, nodes)
        @operator = operator
        @nodes = nodes
      end

      def build(expression)
        @nodes.each_with_index do |node, i|
          node.build(expression)
          expression.append_operator(@operator, 2) if i > 0
        end
      end

      def estimate_size(table)
        estimated_sizes = @nodes.collect do |node|
          node.estimate_size(table)
        end
        case @operator
        when Operator::AND
          estimated_sizes.min
        when Operator::OR
          estimated_sizes.max
        else
          estimated_sizes.first
        end
      end
    end
  end
end
