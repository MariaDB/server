module Groonga
  class ExpressionSizeEstimator
    def initialize(expression, table)
      @expression = expression
      @table = table
      @table_size = @table.size
    end

    def estimate
      builder = ScanInfoBuilder.new(@expression, Operator::OR, 0)
      data_list = builder.build
      return @table_size if data_list.nil?

      or_data_list = group_data_list(data_list)
      or_sizes = or_data_list.collect do |and_data_list|
        and_sizes = and_data_list.collect do |data|
          size = estimate_data(data)
          if data.logical_op == Operator::AND_NOT
            size = @table_size - size
            size = 0 if size < 0
          end
          size
        end
        and_sizes.min
      end
      or_sizes.max
    end

    private
    def group_data_list(data_list)
      or_data_list = [[]]
      data_list.each do |data|
        next if data.op == Operator::NOP

        and_data_list = or_data_list.last
        if and_data_list.empty?
          and_data_list << data
        else
          case data.logical_op
          when Operator::AND, Operator::AND_NOT
            and_data_list << data
          else
            and_data_list = [data]
            or_data_list << and_data_list
          end
        end
      end
      or_data_list
    end

    def estimate_data(data)
      search_index = data.search_indexes.first
      return @table_size if search_index.nil?

      index_column = resolve_index_column(search_index.index_column,
                                          data.op)
      return @table_size if index_column.nil?

      size = nil
      case data.op
      when Operator::MATCH
        size = estimate_match(data, index_column)
      when Operator::REGEXP
        size = estimate_regexp(data, index_column)
      when Operator::EQUAL
        size = estimate_equal(data, index_column)
      when Operator::LESS,
           Operator::LESS_EQUAL,
           Operator::GREATER,
           Operator::GREATER_EQUAL
        size = estimate_range(data, index_column)
      when Operator::CALL
        procedure = data.args.first
        if procedure.is_a?(Procedure) and procedure.name == "between"
          size = estimate_between(data, index_column)
        end
      end
      size || @table_size
    end

    def resolve_index_column(index_column, operator)
      while index_column.is_a?(Accessor)
        index_info = index_column.find_index(operator)
        return nil if index_info.nil?
        index_column = index_info.index
      end

      index_column
    end

    def estimate_match(data, index_column)
      index_column.estimate_size(:query => data.query.value)
    end

    def estimate_regexp(data, index_column)
      index_column.estimate_size(:query => data.query.value,
                                 :mode => data.op)
    end

    def estimate_equal(data, index_column)
      lexicon = index_column.lexicon
      term_id = lexicon[data.query]
      return 0 if term_id.nil?

      index_column.estimate_size(:term_id => term_id)
    end

    def estimate_range(data, index_column)
      lexicon = index_column.lexicon
      value = data.query.value
      options = {}
      case data.op
      when Operator::LESS
        options[:max] = value
        options[:flags] = TableCursorFlags::LT
      when Operator::LESS_EQUAL
        options[:max] = value
        options[:flags] = TableCursorFlags::LE
      when Operator::GREATER
        options[:min] = value
        options[:flags] = TableCursorFlags::GT
      when Operator::GREATER_EQUAL
        options[:min] = value
        options[:flags] = TableCursorFlags::GE
      end
      TableCursor.open(lexicon, options) do |cursor|
        index_column.estimate_size(:lexicon_cursor => cursor)
      end
    end

    def estimate_between(data, index_column)
      lexicon = index_column.lexicon
      _, _, min, min_border, max, max_border = data.args
      options = {
        :min => min,
        :max => max,
        :flags => 0,
      }
      if min_border == "include"
        options[:flags] |= TableCursorFlags::LT
      else
        options[:flags] |= TableCursorFlags::LE
      end
      if max_border == "include"
        options[:flags] |= TableCursorFlags::GT
      else
        options[:flags] |= TableCursorFlags::GE
      end

      TableCursor.open(lexicon, options) do |cursor|
        index_column.estimate_size(:lexicon_cursor => cursor)
      end
    end
  end
end
