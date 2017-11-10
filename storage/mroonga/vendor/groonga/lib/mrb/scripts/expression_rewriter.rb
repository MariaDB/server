module Groonga
  class ExpressionRewriter
    class << self
      def register(name)
        ExpressionRewriters.register(name, self)
      end
    end

    def initialize(expression)
      @expression = expression
    end

    def rewrite
      nil
    end

    private
    def context
      @context ||= Context.instance
    end
  end
end
