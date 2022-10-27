module Groonga
  class QueryLogger
    class Flag
      @@names = {}
      class << self
        def find(name)
          @@names[name]
        end
      end

      attr_reader :name
      def initialize(name, flag)
        @@names[name] = self
        @name = name
        @flag = flag
      end

      def to_i
        @flag
      end

      NONE        = new(:none,        0x00)
      COMMAND     = new(:command,     0x01 << 0)
      RESULT_CODE = new(:result_code, 0x01 << 1)
      DESTINATION = new(:destination, 0x01 << 2)
      CACHE       = new(:cache,       0x01 << 3)
      SIZE        = new(:size,        0x01 << 4)
      SCORE       = new(:score,       0x01 << 5)

      all_flags = COMMAND.to_i |
                  RESULT_CODE.to_i |
                  DESTINATION.to_i |
                  CACHE.to_i |
                  SIZE.to_i |
                  SCORE.to_i
      ALL         = new(:all,         all_flags)
    end
  end
end
