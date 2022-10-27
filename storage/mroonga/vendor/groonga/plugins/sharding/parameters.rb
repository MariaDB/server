module Groonga
  module Sharding
    module Parameters
      @range_index = :auto
      class << self
        attr_accessor :range_index
      end
    end
  end
end
