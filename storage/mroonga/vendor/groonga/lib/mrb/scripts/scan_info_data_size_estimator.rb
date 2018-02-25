module Groonga
  class ScanInfoDataSizeEstimator
    def initialize(data, table)
      @data = data
      @table = table
      @table_size = @table.size
    end

    def estimate
      search_index = @data.search_indexes.first
      return @table_size if search_index.nil?

      index_column = resolve_index_column(search_index.index_column)
      return @table_size if index_column.nil?

      size = nil
      case @data.op
      when Operator::MATCH,
           Operator::FUZZY
        size = estimate_match(index_column)
      when Operator::REGEXP
        size = estimate_regexp(index_column)
      when Operator::EQUAL
        size = estimate_equal(index_column)
      when Operator::LESS,
           Operator::LESS_EQUAL,
           Operator::GREATER,
           Operator::GREATER_EQUAL
        size = estimate_range(index_column)
      when Operator::PREFIX
        size = estimate_prefix(index_column)
      when Operator::CALL
        size = estimate_call(index_column)
      end
      size || @table_size
    end

    private
    def resolve_index_column(index_column)
      while index_column.is_a?(Accessor)
        index_info = index_column.find_index(@data.op)
        return nil if index_info.nil?
        break if index_info.index == index_column
        index_column = index_info.index
      end

      index_column
    end

    def sampling_cursor_limit(n_terms)
      limit = n_terms * 0.01
      if limit < 10
        10
      elsif limit > 1000
        1000
      else
        limit.to_i
      end
    end

    def estimate_match(index_column)
      index_column.estimate_size(:query => @data.query.value)
    end

    def estimate_regexp(index_column)
      index_column.estimate_size(:query => @data.query.value,
                                 :mode => @data.op)
    end

    def estimate_equal(index_column)
      query = @data.query
      if index_column.is_a?(Accessor)
        table = index_column.object
        if index_column.name == "_id"
          if table.id?(query.value)
            1
          else
            0
          end
        else
          if table[query.value]
            1
          else
            0
          end
        end
      else
        lexicon = index_column.lexicon
        if query.domain == lexicon.id
          term_id = query.value
        else
          term_id = lexicon[query]
        end
        return 0 if term_id.nil?

        index_column.estimate_size(:term_id => term_id)
      end
    end

    def estimate_range(index_column)
      if index_column.is_a?(Table)
        is_table_search = true
        lexicon = index_column
      elsif index_column.is_a?(Groonga::Accessor)
        is_table_search = true
        lexicon = index_column.object
      else
        is_table_search = false
        lexicon = index_column.lexicon
      end
      n_terms = lexicon.size
      return 0 if n_terms.zero?

      value = @data.query.value
      options = {
        :limit => sampling_cursor_limit(n_terms),
      }
      case @data.op
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
        if is_table_search
          size = 1
        else
          size = index_column.estimate_size(:lexicon_cursor => cursor)
        end
        size += 1 if cursor.next != ID::NIL
        size
      end
    end

    def estimate_prefix(index_column)
      is_table_search =
        (index_column.is_a?(Accessor) and
        index_column.name == "_key")
      if is_table_search
        lexicon = index_column.object
      else
        lexicon = index_column.lexicon
      end
      n_terms = lexicon.size
      return 0 if n_terms.zero?

      value = @data.query.value
      options = {
        :min => value,
        :limit => sampling_cursor_limit(n_terms),
        :flags => TableCursorFlags::PREFIX,
      }
      TableCursor.open(lexicon, options) do |cursor|
        if is_table_search
          size = 1
        else
          size = index_column.estimate_size(:lexicon_cursor => cursor)
        end
        size += 1 if cursor.next != ID::NIL
        size
      end
    end

    def estimate_call(index_column)
      procedure = @data.args[0]
      arguments = @data.args[1..-1].collect do |arg|
        if arg.is_a?(::Groonga::Object)
          ExpressionTree::Variable.new(arg)
        else
          ExpressionTree::Constant.new(arg)
        end
      end
      node = ExpressionTree::FunctionCall.new(procedure, arguments)
      node.estimate_size(@table)
    end
  end
end
