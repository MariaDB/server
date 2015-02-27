module Groonga
  class GroongaError
    class << self
      def rc
        @rc
      end

      def rc=(rc)
        @rc = rc
      end
    end
  end

  class ErrorMessage < Error
  end
end
