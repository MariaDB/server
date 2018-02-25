module Groonga
  module ID
    # TODO: Should we bind GRN_N_RESERVED_TYPES?
    N_RESERVED_TYPES = 256

    class << self
      def builtin?(id)
        id < N_RESERVED_TYPES
      end
    end
  end
end
