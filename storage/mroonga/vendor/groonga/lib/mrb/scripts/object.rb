module Groonga
  class Object
    def inspect
      super[0..-2] + ": #{grn_inspect}>"
    end
  end
end
