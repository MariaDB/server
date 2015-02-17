module Groonga
  class Database
    def each
      context = Context.instance
      flags =
        TableCursorFlags::ASCENDING |
        TableCursorFlags::BY_ID
      TableCursor.open(self, :flags => flags) do |cursor|
        cursor.each do |id|
          object = context[id]
          yield(object) if object
        end
      end
    end

    def each_table(options={})
      context = Context.instance
      min = options[:prefix]
      flags = 0
      if options[:order] == :descending
        flags |= TableCursorFlags::DESCENDING
      else
        flags |= TableCursorFlags::ASCENDING
      end
      if options[:order_by] == :id
        flags |= TableCursorFlags::BY_ID
      else
        flags |= TableCursorFlags::BY_KEY
      end
      flags |= TableCursorFlags::PREFIX if min
      TableCursor.open(self, :min => min, :flags => flags) do |cursor|
        cursor.each do |id|
          object = context[id]
          yield(object) if object.is_a?(Table)
        end
      end
    end
  end
end
