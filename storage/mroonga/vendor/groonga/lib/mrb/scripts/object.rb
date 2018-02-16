module Groonga
  class Object
    def domain
      Context.instance[domain_id]
    end

    def range
      Context.instance[range_id]
    end

    def corrupt?
      check_corrupt
      false
    rescue
      true
    end
  end
end
