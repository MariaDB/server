module Groonga
  module ExpressionTree
    class Procedure
      attr_reader :object
      def initialize(object)
        @object = object
      end

      def name
        @object.name
      end

      def build(expression)
        expression.append_object(@object, Operator::PUSH, 1)
      end
    end
  end
end
