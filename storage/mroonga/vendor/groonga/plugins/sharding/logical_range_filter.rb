module Groonga
  module Sharding
    class LogicalRangeFilterCommand < Command
      register("logical_range_filter",
               [
                 "logical_table",
                 "shard_key",
                 "min",
                 "min_border",
                 "max",
                 "max_border",
                 "order",
                 "filter",
                 "offset",
                 "limit",
                 "output_columns",
               ])

      def run_body(input)
        enumerator = LogicalEnumerator.new("logical_range_filter", input)
        filter = input[:filter]
        offset = (input[:offset] || 0).to_i
        limit = (input[:limit] || 10).to_i
        output_columns = input[:output_columns] || "_key, *"

        result_sets = []
        n_records = 0
        enumerator.each do |table, shard_key, shard_range|
          result_set = filter_shard(table, filter,
                                    shard_key, shard_range,
                                    enumerator.target_range)
          next if result_set.nil?
          if result_set.empty?
            result_set.close if result_set.temporary?
            next
          end
          result_sets << result_set
          n_records += result_set.size
          break if n_records >= offset + limit
        end

        if result_sets.empty?
          n_elements = 0
        else
          n_elements = 1 # for columns
          result_sets.each do |result_set|
            n_elements += result_set.size
          end
        end

        sort_keys = [
          {
            :key => enumerator.shard_key_name,
            :order => :ascending,
          },
        ]
        current_offset = offset
        current_limit = limit
        writer.array("RESULTSET", n_elements) do
          first_result_set = result_sets.first
          if first_result_set
            writer.write_table_columns(first_result_set, output_columns)
          end
          result_sets.each do |result_set|
            if result_set.size <= current_offset
              current_offset -= result_set.size
              next
            end
            sorted_result_set = result_set.sort(sort_keys,
                                                :offset => current_offset,
                                                :limit => current_limit)
            writer.write_table_records(sorted_result_set, output_columns)
            current_limit -= sorted_result_set.size
            sorted_result_set.close
          end
        end

        result_sets.each do |result_set|
          result_set.close if result_set.temporary?
        end
      end

      def filter_shard(table, filter, shard_key, shard_range, target_range)
        cover_type = target_range.cover_type(shard_range)
        return nil if cover_type == :none

        if cover_type == :all
          if filter.nil?
            return table
          else
            return filter_table(table, filter)
          end
        end

        use_range_index = false
        range_index = nil
        # TODO
        # if filter.nil?
        #   index_info = shard_key.find_index(Operator::LESS)
        #   if index_info
        #     range_index = index_info.index
        #     use_range_index = true
        #   end
        # end

        case cover_type
        when :partial_min
          if use_range_index
            # TODO
            # count_n_records_in_range(range_index,
            #                          target_range.min, target_range.min_border,
            #                          nil, nil)
          else
            filter_table(table, filter) do |expression|
              expression.append_object(shard_key, Operator::PUSH, 1)
              expression.append_operator(Operator::GET_VALUE, 1)
              expression.append_constant(target_range.min, Operator::PUSH, 1)
              if target_range.min_border == :include
                expression.append_operator(Operator::GREATER_EQUAL, 2)
              else
                expression.append_operator(Operator::GREATER, 2)
              end
            end
          end
        when :partial_max
          if use_range_index
            # TODO
            # count_n_records_in_range(range_index,
            #                          nil, nil,
            #                          target_range.max, target_range.max_border)
          else
            filter_table(table, filter) do |expression|
              expression.append_object(shard_key, Operator::PUSH, 1)
              expression.append_operator(Operator::GET_VALUE, 1)
              expression.append_constant(target_range.max, Operator::PUSH, 1)
              if target_range.max_border == :include
                expression.append_operator(Operator::LESS_EQUAL, 2)
              else
                expression.append_operator(Operator::LESS, 2)
              end
            end
          end
        when :partial_min_and_max
          if use_range_index
            # TODO
            # count_n_records_in_range(range_index,
            #                          target_range.min, target_range.min_border,
            #                          target_range.max, target_range.max_border)
          else
            filter_table(table, filter) do |expression|
              expression.append_object(context["between"], Operator::PUSH, 1)
              expression.append_object(shard_key, Operator::PUSH, 1)
              expression.append_operator(Operator::GET_VALUE, 1)
              expression.append_constant(target_range.min, Operator::PUSH, 1)
              expression.append_constant(target_range.min_border,
                                         Operator::PUSH, 1)
              expression.append_constant(target_range.max, Operator::PUSH, 1)
              expression.append_constant(target_range.max_border,
                                         Operator::PUSH, 1)
              expression.append_operator(Operator::CALL, 5)
            end
          end
        end
      end

      def filter_table(table, filter)
        expression = nil
        begin
          expression = Expression.create(table)
          if block_given?
            yield(expression)
            if filter
              expression.parse(filter)
              expression.append_operator(Operator::AND, 2)
            end
          else
            expression.parse(filter)
          end
          table.select(expression)
        ensure
          expression.close if expression
        end
      end
    end
  end
end
