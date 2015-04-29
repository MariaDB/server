module Groonga
  class Object
    def domain
      Context.instance[domain_id]
    end

    def range
      Context.instance[range_id]
    end
  end
end
