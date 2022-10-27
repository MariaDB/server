module Groonga
  class Writer
    def array(name, n_elements)
      open_array(name, n_elements)
      begin
        yield
      ensure
        close_array
      end
    end

    def map(name, n_elements)
      open_map(name, n_elements)
      begin
        yield
      ensure
        close_map
      end
    end
  end
end
