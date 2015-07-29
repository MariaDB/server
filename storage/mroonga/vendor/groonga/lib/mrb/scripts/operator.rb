module Groonga
  class Operator
    @values = {}
    class << self
      def register(operator)
        const_set(operator.name, operator)
        @values[operator.value] = operator
      end

      def find(value)
        @values[value]
      end
    end

    attr_reader :name
    attr_reader :value
    def initialize(name, value)
      @name = name
      @value = value
    end
  end
end
