module Groonga
  class Table
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

    private
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

      sort_key.key = find_column(key_name)
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
