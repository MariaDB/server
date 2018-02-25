module Groonga
  class TableCursor
    include Enumerable

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

    def each
      loop do
        id = self.next
        return if id == Groonga::ID::NIL
        yield(id)
      end
    end
  end
end
