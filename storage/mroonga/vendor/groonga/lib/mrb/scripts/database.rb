module Groonga
  class Database
    def each_raw(options=nil)
      return to_enum(__method__, options) unless block_given?

      if options
        min = options[:prefix]
        order = options[:order]
        order_by = options[:order_by]
      else
        min = nil
        order = :ascending
        order_by = :id
      end
      flags = 0

      if order == :descending
        flags |= TableCursorFlags::DESCENDING
      else
        flags |= TableCursorFlags::ASCENDING
      end
      if order_by == :id
        flags |= TableCursorFlags::BY_ID
      else
        flags |= TableCursorFlags::BY_KEY
      end
      flags |= TableCursorFlags::PREFIX if min
      TableCursor.open(self, :min => min, :flags => flags) do |cursor|
        cursor.each do |id|
          yield(id, cursor)
        end
      end
    end

    def each(options=nil)
      return to_enum(__method__, options) unless block_given?

      context = Context.instance
      each_raw(options) do |id, cursor|
        object = context[id]
        yield(object) if object
      end
    end

    def each_name(options=nil)
      return to_enum(__method__, options) unless block_given?

      each_raw(options) do |id, cursor|
        name = cursor.key
        yield(name)
      end
    end

    def each_table(options={})
      return to_enum(__method__, options) unless block_given?

      context = Context.instance
      each_name(options) do |name|
        next if name.include?(".")
        object = context[name]
        yield(object) if object.is_a?(Table)
      end
    end
  end
end
