module Groonga
  class Table
    include Enumerable
    include Indexable

    def columns
      context = Context.instance
      column_ids.collect do |id|
        context[id]
      end
    end

    def each
      flags =
        TableCursorFlags::ASCENDING |
        TableCursorFlags::BY_ID
      TableCursor.open(self, :flags => flags) do |cursor|
        cursor.each do |id|
          yield(id)
        end
      end
    end

    def sort(keys, options={})
      offset = options[:offset] || 0
      limit = options[:limit] || -1
      ensure_sort_keys(keys) do |sort_keys|
        sorted = Array.create("", self)
        begin
          sort_raw(sort_keys, offset, limit, sorted)
        rescue Exception
          sorted.close
          raise
        end
        sorted
      end
    end

    def group(keys, result)
      ensure_sort_keys(keys) do |sort_keys|
        group_raw(sort_keys, result)
      end
    end

    def apply_window_function(output_column,
                              window_function_call,
                              options={})
      ensure_sort_keys_accept_nil(options[:sort_keys]) do |sort_keys|
        ensure_sort_keys_accept_nil(options[:group_keys]) do |group_keys|
          window_definition = WindowDefinition.new
          begin
            window_definition.sort_keys = sort_keys
            window_definition.group_keys = group_keys
            apply_window_function_raw(output_column,
                                      window_definition,
                                      window_function_call)
          ensure
            window_definition.close
          end
        end
      end
    end

    private
    def ensure_sort_keys_accept_nil(keys, &block)
      return yield(nil) if keys.nil?

      ensure_sort_keys(keys, &block)
    end

    def ensure_sort_keys(keys)
      if keys.is_a?(::Array) and keys.all? {|key| key.is_a?(TableSortKey)}
        return yield(keys)
      end

      converted_keys = []

      begin
        keys = [keys] unless keys.is_a?(::Array)
        sort_keys = keys.collect do |key|
          ensure_sort_key(key, converted_keys)
        end
        yield(sort_keys)
      ensure
        converted_keys.each do |converted_key|
          converted_key.close
        end
      end
    end

    def ensure_sort_key(key, converted_keys)
      return key if key.is_a?(TableSortKey)

      sort_key = TableSortKey.new
      converted_keys << sort_key

      key_name = nil
      order = :ascending
      offset = 0
      if key.is_a?(::Hash)
        key_name = key[:key]
        order    = key[:order] || order
        offset   = key[:offset] || offset
      else
        key_name = key
      end

      case key_name
      when String
        # Do nothing
      when Symbol
        key_name = key_name.to_s
      else
        message = "sort key name must be String or Symbol: " +
                  "#{key_name.inspect}: #{key.inspect}"
        raise ArgumentError, message
      end

      if key_name.start_with?("-")
        key_name[0] = ""
        order = :descending
      elsif key_name.start_with?("+")
        key_name[0] = ""
      end

      key = find_column(key_name)
      if key.nil?
        table_name = name || "(temporary)"
        message = "unknown key: #{key_name.inspect}: "
        message << "#{table_name}(#{size})"
        raise ArgumentError, message
      end

      sort_key.key = key
      if order == :ascending
        sort_key.flags = Groonga::TableSortFlags::ASCENDING
      else
        sort_key.flags = Groonga::TableSortFlags::DESCENDING
      end
      sort_key.offset = offset
      sort_key
    end
  end
end
