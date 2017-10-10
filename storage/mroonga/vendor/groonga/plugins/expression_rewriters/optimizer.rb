module Groonga
  module ExpressionRewriters
    class Optimizer < ExpressionRewriter
      register "optimizer"

      def rewrite
        builder = ExpressionTreeBuilder.new(@expression)
        root_node = builder.build

        variable = @expression[0]
        table = context[variable.domain]
        optimized_root_node = optimize_node(table, root_node)

        rewritten = Expression.create(table)
        optimized_root_node.build(rewritten)
        rewritten
      end

      private
      def optimize_node(table, node)
        case node
        when ExpressionTree::LogicalOperation
          optimized_sub_nodes = node.nodes.collect do |sub_node|
            optimize_node(table, sub_node)
          end
          case node.operator
          when Operator::AND
            optimized_sub_nodes =
              optimize_and_sub_nodes(table, optimized_sub_nodes)
          end
          ExpressionTree::LogicalOperation.new(node.operator,
                                               optimized_sub_nodes)
        when ExpressionTree::BinaryOperation
          optimized_left = optimize_node(table, node.left)
          optimized_right = optimize_node(table, node.right)
          if optimized_left.is_a?(ExpressionTree::Constant) and
              optimized_right.is_a?(ExpressionTree::Variable)
            ExpressionTree::BinaryOperation.new(node.operator,
                                                optimized_right,
                                                optimized_left)
          elsif node.left == optimized_left and node.right == optimized_right
            node
          else
            ExpressionTree::BinaryOperation.new(node.operator,
                                                optimized_left,
                                                optimized_right)
          end
        else
          node
        end
      end

      def optimize_and_sub_nodes(table, sub_nodes)
        grouped_sub_nodes = sub_nodes.group_by do |sub_node|
          case sub_node
          when ExpressionTree::BinaryOperation
            if sub_node.left.is_a?(ExpressionTree::Variable)
              sub_node.left.column
            else
              nil
            end
          else
            nil
          end
        end

        optimized_nodes = []
        grouped_sub_nodes.each do |column, grouped_nodes|
          if column
            grouped_nodes = optimize_grouped_nodes(column, grouped_nodes)
          end
          optimized_nodes.concat(grouped_nodes)
        end

        optimized_nodes.sort_by do |node|
          node.estimate_size(table)
        end
      end

      COMPARISON_OPERATORS = [
        Operator::EQUAL,
        Operator::NOT_EQUAL,
        Operator::LESS,
        Operator::GREATER,
        Operator::LESS_EQUAL,
        Operator::GREATER_EQUAL,
      ]
      def optimize_grouped_nodes(column, grouped_nodes)
        target_nodes, done_nodes = grouped_nodes.partition do |node|
          node.is_a?(ExpressionTree::BinaryOperation) and
            COMPARISON_OPERATORS.include?(node.operator) and
            node.right.is_a?(ExpressionTree::Constant)
        end

        # TODO: target_nodes = remove_needless_nodes(target_nodes)
        # e.g.: x < 1 && x < 3 -> x < 1: (x < 3) is meaningless

        if target_nodes.size == 2
          between_node = try_optimize_between(column, target_nodes)
          if between_node
            done_nodes << between_node
          else
            done_nodes.concat(target_nodes)
          end
        else
          done_nodes.concat(target_nodes)
        end

        done_nodes
      end

      def try_optimize_between(column, target_nodes)
        greater_node = nil
        less_node = nil
        target_nodes.each do |node|
          case node.operator
          when Operator::GREATER, Operator::GREATER_EQUAL
            greater_node = node
          when Operator::LESS, Operator::LESS_EQUAL
            less_node = node
          end
        end
        return nil if greater_node.nil? or less_node.nil?

        between = ExpressionTree::Procedure.new(context["between"])
        if greater_node.operator == Operator::GREATER
          greater_border = "exclude"
        else
          greater_border = "include"
        end
        if less_node.operator == Operator::LESS
          less_border = "exclude"
        else
          less_border = "include"
        end
        arguments = [
          ExpressionTree::Variable.new(column),
          greater_node.right,
          ExpressionTree::Constant.new(greater_border),
          less_node.right,
          ExpressionTree::Constant.new(less_border),
        ]
        ExpressionTree::FunctionCall.new(between, arguments)
      end
    end
  end
end
