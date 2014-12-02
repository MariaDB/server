module Groonga
  class Context
    class ErrorLevel
      @@names = {}

      class << self
        def find(name)
          @@names[name]
        end
      end

      attr_reader :name
      def initialize(name, level)
        @@names[name] = self
        @name  = name
        @level = level
      end

      def to_i
        @level
      end

      EMERGENCY = new(:emergency, 1)
      ALERT     = new(:alert,     2)
      CRITICAL  = new(:critical,  3)
      ERROR     = new(:error,     4)
      WARNING   = new(:warning,   5)
    end
  end
end
