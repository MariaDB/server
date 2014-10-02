module Groonga
  class IndexInfo
    attr_reader :index
    attr_reader :section_id
    def initialize(index, section_id)
      @index = index
      @section_id = section_id
    end
  end
end
