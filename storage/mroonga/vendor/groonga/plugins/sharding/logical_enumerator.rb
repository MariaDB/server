module Groonga
  module Sharding
    class LogicalEnumerator
      attr_reader :target_range
      attr_reader :logical_table
      attr_reader :shard_key_name
      def initialize(command_name, input)
        @command_name = command_name
        @input = input
        initialize_parameters
      end

      def each(&block)
        each_internal(:ascending, &block)
      end

      def reverse_each(&block)
        each_internal(:descending, &block)
      end

      private
      def each_internal(order)
        context = Context.instance
        each_shard_with_around(order) do |prev_shard, current_shard, next_shard|
          table = current_shard.table
          shard_range_data = current_shard.range_data
          shard_range = nil

          if shard_range_data.day.nil?
            if order == :ascending
              if next_shard
                next_shard_range_data = next_shard.range_data
              else
                next_shard_range_data = nil
              end
            else
              if prev_shard
                next_shard_range_data = prev_shard.range_data
              else
                next_shard_range_data = nil
              end
            end
            max_day = compute_month_shard_max_day(shard_range_data.year,
                                                  shard_range_data.month,
                                                  next_shard_range_data)
            shard_range = MonthShardRange.new(shard_range_data.year,
                                              shard_range_data.month,
                                              max_day)
          else
            shard_range = DayShardRange.new(shard_range_data.year,
                                            shard_range_data.month,
                                            shard_range_data.day)
          end

          physical_shard_key_name = "#{table.name}.#{@shard_key_name}"
          shard_key = context[physical_shard_key_name]
          if shard_key.nil?
            message =
              "[#{@command_name}] shard_key doesn't exist: " +
              "<#{physical_shard_key_name}>"
            raise InvalidArgument, message
          end

          yield(table, shard_key, shard_range)
        end
      end

      def each_shard_with_around(order)
        context = Context.instance
        prefix = "#{@logical_table}_"

        shards = [nil]
        context.database.each_table(:prefix => prefix,
                                    :order_by => :key,
                                    :order => order) do |table|
          shard_range_raw = table.name[prefix.size..-1]

          case shard_range_raw
          when /\A(\d{4})(\d{2})\z/
            shard_range_data = ShardRangeData.new($1.to_i, $2.to_i, nil)
          when /\A(\d{4})(\d{2})(\d{2})\z/
            shard_range_data = ShardRangeData.new($1.to_i, $2.to_i, $3.to_i)
          else
            next
          end

          shards << Shard.new(table, shard_range_data)
          next if shards.size < 3
          yield(*shards)
          shards.shift
        end

        if shards.size == 2
          yield(shards[0], shards[1], nil)
        end
      end

      private
      def initialize_parameters
        @logical_table = @input[:logical_table]
        if @logical_table.nil?
          raise InvalidArgument, "[#{@command_name}] logical_table is missing"
        end

        @shard_key_name = @input[:shard_key]
        if @shard_key_name.nil?
          raise InvalidArgument, "[#{@command_name}] shard_key is missing"
        end

        @target_range = TargetRange.new(@command_name, @input)
      end

      def compute_month_shard_max_day(year, month, next_shard_range)
        return nil if next_shard_range.nil?

        return nil if month != next_shard_range.month

        next_shard_range.day
      end

      class Shard
        attr_reader :table, :range_data
        def initialize(table, range_data)
          @table = table
          @range_data = range_data
        end
      end

      class ShardRangeData
        attr_reader :year, :month, :day
        def initialize(year, month, day)
          @year = year
          @month = month
          @day = day
        end
      end

      class DayShardRange
        attr_reader :year, :month, :day
        def initialize(year, month, day)
          @year = year
          @month = month
          @day = day
        end

        def least_over_time
          Time.local(@year, @month, @day + 1)
        end

        def min_time
          Time.local(@year, @month, @day)
        end

        def include?(time)
          @year == time.year and
            @month == time.month and
            @day == time.day
        end
      end

      class MonthShardRange
        attr_reader :year, :month, :max_day
        def initialize(year, month, max_day)
          @year = year
          @month = month
          @max_day = max_day
        end

        def least_over_time
          if @max_day.nil?
            Time.local(@year, @month + 1, 1)
          else
            Time.local(@year, @month, @max_day + 1)
          end
        end

        def min_time
          Time.local(@year, @month, 1)
        end

        def include?(time)
          return false unless @year == time.year
          return false unless @month == time.month

          if @max_day.nil?
            true
          else
            time.day <= @max_day
          end
        end
      end

      class TargetRange
        attr_reader :min, :min_border
        attr_reader :max, :max_border
        def initialize(command_name, input)
          @command_name = command_name
          @input = input
          @min = parse_value(:min)
          @min_border = parse_border(:min_border)
          @max = parse_value(:max)
          @max_border = parse_border(:max_border)
        end

        def cover_type(shard_range)
          return :all if @min.nil? and @max.nil?

          if @min and @max
            return :none unless in_min?(shard_range)
            return :none unless in_max?(shard_range)
            min_partial_p = in_min_partial?(shard_range)
            max_partial_p = in_max_partial?(shard_range)
            if min_partial_p and max_partial_p
              :partial_min_and_max
            elsif min_partial_p
              :partial_min
            elsif max_partial_p
              :partial_max
            else
              :all
            end
          elsif @min
            return :none unless in_min?(shard_range)
            if in_min_partial?(shard_range)
              :partial_min
            else
              :all
            end
          else
            return :none unless in_max?(shard_range)
            if in_max_partial?(shard_range)
              :partial_max
            else
              :all
            end
          end
        end

        private
        def parse_value(name)
          value = @input[name]
          return nil if value.nil?

          Converter.convert(value, Time)
        end

        def parse_border(name)
          border = @input[name]
          return :include if border.nil?

          case border
          when "include"
            :include
          when "exclude"
            :exclude
          else
            message =
              "[#{@command_name}] #{name} must be \"include\" or \"exclude\": " +
              "<#{border}>"
            raise InvalidArgument, message
          end
        end

        def in_min?(shard_range)
          @min < shard_range.least_over_time
        end

        def in_min_partial?(shard_range)
          return false unless shard_range.include?(@min)

          return true if @min_border == :exclude

          not (@min.hour == 0 and
               @min.min  == 0 and
               @min.sec  == 0 and
               @min.usec == 0)
        end

        def in_max?(shard_range)
          max_base_time = shard_range.min_time
          if @max_border == :include
            @max >= max_base_time
          else
            @max > max_base_time
          end
        end

        def in_max_partial?(shard_range)
          shard_range.include?(@max)
        end
      end
    end
  end
end
