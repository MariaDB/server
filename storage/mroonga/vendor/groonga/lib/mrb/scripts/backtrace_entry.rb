module Groonga
  class BacktraceEntry
    class << self
      def parse(entry)
        match_data = /:(\d+):?/.match(entry)
        file = match_data.pre_match
        line = match_data[1].to_i
        detail_match_data = /\A(in )?(\S+)\s*/.match(match_data.post_match)
        if detail_match_data[1]
          method = detail_match_data[2]
          message = detail_match_data.post_match
        else
          method = ""
          message = match_data.post_match
        end
        new(file, line, method, message)
      end
    end

    attr_reader :file, :line, :method, :message
    def initialize(file, line, method, message)
      @file = file
      @line = line
      @method = method
      @message = message
    end
  end
end
