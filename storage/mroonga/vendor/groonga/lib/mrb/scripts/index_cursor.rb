module Groonga
  class IndexCursor
    class << self
      def open(*arguments)
        cursor = open_raw(*arguments)
        if block_given?
          begin
            yield(cursor)
          ensure
            cursor.close
          end
        else
          cursor
        end
      end
    end
  end
end
