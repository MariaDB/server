module Groonga
  module Sharding
    class LogicalEnumerator
      attr_reader :target_range
      attr_reader :shard_key_name
      def initialize(command_name, input)
        @command_name = command_name
        @input = input
        initialize_parameters
      end

      def each
        prefix = "#{@logical_table}_"
        context = Context.instance
        context.database.each_table(:prefix => prefix,
                                    :order_by => :key,
                                    :order => :ascending) do |table|
          shard_range_raw = table.name[prefix.size..-1]

          next unless /\A(\d{4})(\d{2})(\d{2})\z/ =~ shard_range_raw
          shard_range = ShardRange.new($1.to_i, $2.to_i, $3.to_i)

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

      class ShardRange
        attr_reader :year, :month, :day
        def initialize(year, month, day)
          @year = year
          @month = month
          @day = day
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
          base_time = Time.local(shard_range.year,
                                 shard_range.month,
                                 shard_range.day + 1)
          @min < base_time
        end

        def in_min_partial?(shard_range)
          return false unless @min.year == shard_range.year
          return false unless @min.month == shard_range.month
          return false unless @min.day == shard_range.day

          return true if @min_border == :exclude

          @min.hour != 0 and
            @min.min != 0 and
            @min.sec != 0 and
            @min.usec != 0
        end

        def in_max?(shard_range)
          max_base_time = Time.local(shard_range.year,
                                     shard_range.month,
                                     shard_range.day)
          if @max_border == :include
            @max >= max_base_time
          else
            @max > max_base_time
          end
        end

        def in_max_partial?(shard_range)
          @max.year == shard_range.year and
            @max.month == shard_range.month and
            @max.day == shard_range.day
        end
      end
    end
  end
end
