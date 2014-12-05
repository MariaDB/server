module Groonga
  class Logger
    class Level
      @@names = {}
      def self.find(name)
        @@names[name]
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

      NONE    = new(:none,    0)
      EMERG   = new(:emerg,   1)
      ALERT   = new(:alert,   2)
      CRIT    = new(:crit,    3)
      ERROR   = new(:error,   4)
      WARNING = new(:warning, 5)
      NOTICE  = new(:notice,  6)
      INFO    = new(:info,    7)
      DEBUG   = new(:debug,   8)
      DUMP    = new(:dump,    9)
    end
  end
end
