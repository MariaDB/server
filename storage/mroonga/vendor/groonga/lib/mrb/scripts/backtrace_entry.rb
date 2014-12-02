module Groonga
  class BacktraceEntry
    class << self
      def parse(entry)
        match_data = /:(\d+):?/.match(entry)
        file = match_data.pre_match
        line = match_data[1].to_i
        method = match_data.post_match.gsub(/\Ain /, "")
        new(file, line, method)
      end
    end

    attr_reader :file, :line, :method
    def initialize(file, line, method)
      @file = file
      @line = line
      @method = method
    end
  end
end
