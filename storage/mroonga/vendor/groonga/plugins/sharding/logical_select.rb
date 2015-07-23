module Groonga
  module Sharding
    class LogicalSelectCommand < Command
      register("logical_select",
               [
                 "logical_table",
                 "shard_key",
                 "min",
                 "min_border",
                 "max",
                 "max_border",
                 "filter",
                 "sortby",
                 "output_columns",
                 "offset",
                 "limit",
                 "drilldown",
                 "drilldown_sortby",
                 "drilldown_output_columns",
                 "drilldown_offset",
                 "drilldown_limit",
               ])

      def run_body(input)
        enumerator = LogicalEnumerator.new("logical_select", input)

        context = ExecuteContext.new(input)
        begin
          executor = Executor.new(context)
          executor.execute

          n_results = 1
          drilldowns = context.drilldown.result_sets
          n_results += drilldowns.size

          writer.array("RESULT", n_results) do
            write_records(writer, context)
            write_drilldowns(writer, context, drilldowns)
          end
        ensure
          context.close
        end
      end

      private
      def write_records(writer, context)
        result_sets = context.result_sets

        n_hits = 0
        n_elements = 2 # for N hits and columns
        result_sets.each do |result_set|
          n_hits += result_set.size
          n_elements += result_set.size
        end

        output_columns = context.output_columns

        writer.array("RESULTSET", n_elements) do
          writer.array("NHITS", 1) do
            writer.write(n_hits)
          end
          first_result_set = result_sets.first
          if first_result_set
            writer.write_table_columns(first_result_set, output_columns)
          end

          current_offset = context.offset
          current_offset += n_hits if current_offset < 0
          current_limit = context.limit
          current_limit += n_hits + 1 if current_limit < 0
          options = {
            :offset => current_offset,
            :limit => current_limit,
          }
          result_sets.each do |result_set|
            if result_set.size > current_offset
              writer.write_table_records(result_set, output_columns, options)
            end
            if current_offset > 0
              current_offset = [current_offset - result_set.size, 0].max
            end
            current_limit -= result_set.size
            break if current_limit <= 0
            options[:offset] = current_offset
            options[:limit] = current_limit
          end
        end
      end

      def write_drilldowns(writer, context, drilldowns)
        output_columns = context.drilldown.output_columns

        options = {
          :offset => context.drilldown.output_offset,
          :limit  => context.drilldown.limit,
        }

        drilldowns.each do |drilldown|
          n_elements = 2 # for N hits and columns
          n_elements += drilldown.size
          writer.array("RESULTSET", n_elements) do
            writer.array("NHITS", 1) do
              writer.write(drilldown.size)
            end
            writer.write_table_columns(drilldown, output_columns)
            writer.write_table_records(drilldown, output_columns,
                                       options)
          end
        end
      end

      module KeysParsable
        private
        def parse_keys(raw_keys)
          return [] if raw_keys.nil?

          raw_keys.strip.split(/ *, */)
        end
      end

      class ExecuteContext
        include KeysParsable

        attr_reader :enumerator
        attr_reader :filter
        attr_reader :offset
        attr_reader :limit
        attr_reader :sort_keys
        attr_reader :output_columns
        attr_reader :result_sets
        attr_reader :drilldown
        def initialize(input)
          @input = input
          @enumerator = LogicalEnumerator.new("logical_select", @input)
          @filter = @input[:filter]
          @offset = (@input[:offset] || 0).to_i
          @limit = (@input[:limit] || 10).to_i
          @sort_keys = parse_keys(@input[:sortby])
          @output_columns = @input[:output_columns] || "_key, *"

          @result_sets = []

          @drilldown = DrilldownExecuteContext.new(@input)
        end

        def close
          @result_sets.each do |result_set|
            result_set.close if result_set.temporary?
          end

          @drilldown.close
        end
      end

      class DrilldownExecuteContext
        include KeysParsable

        attr_reader :keys
        attr_reader :offset
        attr_reader :limit
        attr_reader :sort_keys
        attr_reader :output_columns
        attr_reader :output_offset
        attr_reader :result_sets
        attr_reader :unsorted_result_sets
        def initialize(input)
          @input = input
          @keys = parse_keys(@input[:drilldown])
          @offset = (@input[:drilldown_offset] || 0).to_i
          @limit = (@input[:drilldown_limit] || 10).to_i
          @sort_keys = parse_keys(@input[:drilldown_sortby])
          @output_columns = @input[:drilldown_output_columns]
          @output_columns ||= "_key, _nsubrecs"

          if @sort_keys.empty?
            @output_offset = @offset
          else
            @output_offset = 0
          end

          @result_sets = []
          @unsorted_result_sets = []
        end

        def close
          @result_sets.each do |result_set|
            result_set.close
          end
          @unsorted_result_sets.each do |result_set|
            result_set.close
          end
        end
      end

      class Executor
        def initialize(context)
          @context = context
        end

        def execute
          execute_search
          execute_drilldown
        end

        private
        def execute_search
          first_table = nil
          enumerator = @context.enumerator
          enumerator.each do |table, shard_key, shard_range|
            first_table ||= table
            next if table.empty?

            shard_executor = ShardExecutor.new(@context,
                                               table, shard_key, shard_range)
            shard_executor.execute
          end
          if first_table.nil?
            message =
              "[logical_select] no shard exists: " +
              "logical_table: <#{enumerator.logical_table}>: " +
              "shard_key: <#{enumerator.shard_key_name}>"
            raise InvalidArgument, message
          end
          if @context.result_sets.empty?
            result_set = HashTable.create(:flags => ObjectFlags::WITH_SUBREC,
                                          :key_type => first_table)
            @context.result_sets << result_set
          end
        end

        def execute_drilldown
          drilldown = @context.drilldown
          group_result = TableGroupResult.new
          sort_options = {
            :offset => drilldown.offset,
            :limit  => drilldown.limit,
          }
          begin
            group_result.key_begin = 0
            group_result.key_end = 0
            group_result.limit = 1
            group_result.flags = TableGroupFlags::CALC_COUNT
            drilldown.keys.each do |key|
              @context.result_sets.each do |result_set|
                result_set.group([key], group_result)
              end
              result_set = group_result.table
              if drilldown.sort_keys.empty?
                drilldown.result_sets << result_set
              else
                drilldown.result_sets << result_set.sort(drilldown.sort_keys,
                                                         sort_options)
                drilldown.unsorted_result_sets << result_set
              end
              group_result.table = nil
            end
          ensure
            group_result.close
          end
        end
      end

      class ShardExecutor
        def initialize(context, table, shard_key, shard_range)
          @context = context
          @table = table
          @shard_key = shard_key
          @shard_range = shard_range

          @filter = @context.filter
          @result_sets = @context.result_sets

          @target_range = @context.enumerator.target_range

          @cover_type = @target_range.cover_type(@shard_range)

          @expression_builder = RangeExpressionBuilder.new(@shard_key,
                                                           @target_range,
                                                           @filter)
        end

        def execute
          return if @cover_type == :none

          case @cover_type
          when :all
            filter_shard_all
          when :partial_min
            filter_table do |expression|
              @expression_builder.build_partial_min(expression)
            end
          when :partial_max
            filter_table do |expression|
              @expression_builder.build_partial_max(expression)
            end
          when :partial_min_and_max
            filter_table do |expression|
              @expression_builder.build_partial_min_and_max(expression)
            end
          end
        end

        private
        def filter_shard_all
          if @filter.nil?
            @result_sets << @table
          else
            filter_table do |expression|
              @expression_builder.build_all(expression)
            end
          end
        end

        def create_expression(table)
          expression = Expression.create(table)
          begin
            yield(expression)
          ensure
            expression.close
          end
        end

        def filter_table
          create_expression(@table) do |expression|
            yield(expression)
            @result_sets << @table.select(expression)
          end
        end
      end
    end
  end
end
