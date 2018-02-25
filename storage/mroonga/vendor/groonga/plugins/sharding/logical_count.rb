module Groonga
  module Sharding
    class LogicalCountCommand < Command
      register("logical_count",
               [
                 "logical_table",
                 "shard_key",
                 "min",
                 "min_border",
                 "max",
                 "max_border",
                 "filter",
               ])

      def run_body(input)
        enumerator = LogicalEnumerator.new("logical_count", input)
        filter = input[:filter]

        total = 0
        enumerator.each do |shard, shard_range|
          total += count_n_records(filter, shard, shard_range,
                                   enumerator.target_range)
        end
        writer.write(total)
      end

      private
      def cache_key(input)
        key = "logical_count\0"
        key << "#{input[:logical_table]}\0"
        key << "#{input[:shard_key]}\0"
        key << "#{input[:min]}\0"
        key << "#{input[:min_border]}\0"
        key << "#{input[:max]}\0"
        key << "#{input[:max_border]}\0"
        key << "#{input[:filter]}\0"
        key
      end

      def log_use_range_index(use, table_name, line, method)
        message = "[logical_count]"
        if use
          message << "[range-index]"
        else
          message << "[select]"
        end
        message << " <#{table_name}>"
        Context.instance.logger.log(Logger::Level::DEBUG,
                                    __FILE__,
                                    line,
                                    method.to_s,
                                    message)
      end

      def count_n_records(filter, shard, shard_range, target_range)
        cover_type = target_range.cover_type(shard_range)
        return 0 if cover_type == :none

        shard_key = shard.key
        if shard_key.nil?
          message = "[logical_count] shard_key doesn't exist: " +
                    "<#{shard.key_name}>"
          raise InvalidArgument, message
        end
        table = shard.table
        table_name = shard.table_name

        expression_builder = RangeExpressionBuilder.new(shard_key,
                                                        target_range)
        expression_builder.filter = filter
        if cover_type == :all
          log_use_range_index(false, table_name, __LINE__, __method__)
          if filter.nil?
            return table.size
          else
            return filtered_count_n_records(table) do |expression|
              expression_builder.build_all(expression)
            end
          end
        end

        range_index = nil
        if filter.nil?
          index_info = shard_key.find_index(Operator::LESS)
          if index_info
            range_index = index_info.index
          end
        end

        use_range_index = (!range_index.nil?)
        log_use_range_index(use_range_index, table_name, __LINE__, __method__)

        case cover_type
        when :partial_min
          if range_index
            count_n_records_in_range(range_index,
                                     target_range.min, target_range.min_border,
                                     nil, nil)
          else
            filtered_count_n_records(table) do |expression|
              expression_builder.build_partial_min(expression)
            end
          end
        when :partial_max
          if range_index
            count_n_records_in_range(range_index,
                                     nil, nil,
                                     target_range.max, target_range.max_border)
          else
            filtered_count_n_records(table) do |expression|
              expression_builder.build_partial_max(expression)
            end
          end
        when :partial_min_and_max
          if range_index
            count_n_records_in_range(range_index,
                                     target_range.min, target_range.min_border,
                                     target_range.max, target_range.max_border)
          else
            filtered_count_n_records(table) do |expression|
              expression_builder.build_partial_min_and_max(expression)
            end
          end
        end
      end

      def filtered_count_n_records(table)
        expression = nil
        filtered_table = nil

        begin
          expression = Expression.create(table)
          yield(expression)
          filtered_table = table.select(expression)
          filtered_table.size
        ensure
          filtered_table.close if filtered_table
          expression.close if expression
        end
      end

      def count_n_records_in_range(range_index,
                                   min, min_border, max, max_border)
        flags = TableCursorFlags::BY_KEY
        case min_border
        when :include
          flags |= TableCursorFlags::GE
        when :exclude
          flags |= TableCursorFlags::GT
        end
        case max_border
        when :include
          flags |= TableCursorFlags::LE
        when :exclude
          flags |= TableCursorFlags::LT
        end

        TableCursor.open(range_index.table,
                         :min => min,
                         :max => max,
                         :flags => flags) do |table_cursor|
          IndexCursor.open(table_cursor, range_index) do |index_cursor|
            index_cursor.count
          end
        end
      end
    end
  end
end
