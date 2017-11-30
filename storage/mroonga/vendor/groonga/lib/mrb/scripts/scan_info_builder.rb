require "scan_info_data"

module Groonga
  class ScanInfoBuilder
    def initialize(expression, operator, record_exist)
      @data_list = []
      @expression = expression
      @operator = operator
      @record_exist = record_exist
      @variable = @expression[0]
      @table = Context.instance[@variable.domain]
    end

    RELATION_OPERATORS = [
      Operator::MATCH,
      Operator::NEAR,
      Operator::NEAR2,
      Operator::SIMILAR,
      Operator::PREFIX,
      Operator::SUFFIX,
      Operator::EQUAL,
      Operator::NOT_EQUAL,
      Operator::LESS,
      Operator::GREATER,
      Operator::LESS_EQUAL,
      Operator::GREATER_EQUAL,
      Operator::GEO_WITHINP5,
      Operator::GEO_WITHINP6,
      Operator::GEO_WITHINP8,
      Operator::TERM_EXTRACT,
      Operator::REGEXP,
      Operator::FUZZY,
    ]

    ARITHMETIC_OPERATORS = [
      Operator::BITWISE_OR,
      Operator::BITWISE_XOR,
      Operator::BITWISE_AND,
      Operator::BITWISE_NOT,
      Operator::SHIFTL,
      Operator::SHIFTR,
      Operator::SHIFTRR,
      Operator::PLUS,
      Operator::MINUS,
      Operator::STAR,
      Operator::MOD,
    ]

    LOGICAL_OPERATORS = [
      Operator::AND,
      Operator::OR,
      Operator::AND_NOT,
      Operator::ADJUST,
    ]
    def build
      return nil unless valid?

      context = BuildContext.new(@expression)
      codes = context.codes
      while context.have_next?
        code = context.code
        code_op = context.code_op
        i = context.i
        context.next

        case code_op
        when *RELATION_OPERATORS
          context.status = :start
          data = context.data
          data.op = code_op
          data.end = i
          data.weight = code.value.value if code.value
          data.match_resolve_index
          @data_list << data
          context.data = nil
        when *LOGICAL_OPERATORS
          if context.status == :const
            data = context.data
            data.op = Operator::PUSH
            data.end = data.start
            @data_list << data
            context.data = nil
          end
          put_logical_op(code_op, i)
          # TODO: rescue and return nil
          context.status = :start
        when Operator::PUSH
          context.data ||= ScanInfoData.new(i)
          data = context.data
          if code.value == @variable
            context.status = :var
          else
            data.args << code.value
            if context.status == :start
              data.flags |= ScanInfo::Flags::PRE_CONST
            end
            context.status = :const
          end
          if code.modify > 0 and
              LOGICAL_OPERATORS.include?(codes[i + code.modify].op)
            data.op = Operator::PUSH
            data.end = data.start
            @data_list << data
            context.data = nil
            context.status = :start
          end
        when Operator::GET_VALUE
          case context.status
          when :start
            context.data ||= ScanInfoData.new(i)
            data = context.data
            context.status = :column1
            data.args << code.value
          when :const, :var
            context.status = :column1
            data.args << code.value
          when :column1
            message = "invalid expression: can't use column as a value: "
            message << "<#{code.value.name}>: <#{@expression.grn_inspect}>"
            raise ErrorMessage, message
          when :column2
            # Do nothing
          else
            message = "internal expression parsing error: unknown status: "
            message << "<#{context.status.inspect}>: "
            message << "<#{@expression.grn_inspect}>"
            raise ErrorMessage, message
          end
        when Operator::CALL
          context.data ||= ScanInfoData.new(i)
          data = context.data
          if (code.flags & ExpressionCode::Flags::RELATIONAL_EXPRESSION) != 0 or
              (not context.have_next?)
            context.status = :start
            data.op = code_op
            data.end = i
            data.call_relational_resolve_indexes
            @data_list << data
            context.data = nil
          else
            context.status = :column2
          end
        when Operator::GET_REF
          context.data ||= ScanInfoData.new(i)
          case context.status
          when :start
            data = context.data
            context.status = :column1
            data.args << code.value
          end
        when Operator::GET_MEMBER
          data = context.data
          index = data.args.pop
          data.start_position = index.value
          context.status = :column1
        when Operator::NOT
          success = build_not(context, code, i)
          return nil unless success
        end
      end

      if @operator == Operator::OR and !@record_exist
        first_data = @data_list.first
        if (first_data.flags & ScanInfo::Flags::PUSH) == 0 or
            first_data.logical_op != @operator
          raise ErrorMessage, "invalid expr"
        else
          first_data.flags &= ~ScanInfo::Flags::PUSH
          first_data.logical_op = @operator
        end
      else
        put_logical_op(@operator, context.n_codes)
      end

      optimize
    end

    private
    def valid?
      n_relation_expressions = 0
      n_logical_expressions = 0
      status = :start
      codes = @expression.codes
      codes.each_with_index do |code, i|
        case code.op
        when *RELATION_OPERATORS
          if status == :start || status == :var
            return false
          end
          status = :start
          n_relation_expressions += 1
        when *ARITHMETIC_OPERATORS
          if status == :start || status == :var
            return false
          end
          status = :start
          return false if n_relation_expressions != (n_logical_expressions + 1)
        when *LOGICAL_OPERATORS
          case status
          when :start
            n_logical_expressions += 1
            return false if n_logical_expressions >= n_relation_expressions
          when :const
            n_logical_expressions += 1
            n_relation_expressions += 1
            return false if n_logical_expressions >= n_relation_expressions
            status = :start
          else
            return false
          end
        when Operator::PUSH
          if code.modify > 0 and
              LOGICAL_OPERATORS.include?(codes[i + code.modify].op)
            n_relation_expressions += 1
            status = :start
          else
            if code.value == @variable
              status = :var
            else
              status = :const
            end
          end
        when Operator::GET_VALUE
          case status
          when :start, :const, :var
            status = :column1
          when :column1
            status = :column2
          when :column2
            # Do nothing
          else
            return false
          end
        when Operator::CALL
          if (code.flags & ExpressionCode::Flags::RELATIONAL_EXPRESSION) != 0 or
              code == codes.last
            status = :start
            n_relation_expressions += 1
          else
            status = :column2
          end
        when Operator::GET_REF
          case status
          when :start
            status = :column1
          else
            return false
          end
        when Operator::GET_MEMBER
          case status
          when :const
            return false unless codes[i - 1].value.value.is_a?(Integer)
            status = :column1
          else
            return false
          end
        when Operator::NOT
          # Do nothing
        else
          return false
        end
      end

      return false if status != :start
      return false if n_relation_expressions != (n_logical_expressions + 1)

      true
    end

    def put_logical_op(operator, start)
      n_parens = 1
      n_dif_ops = 0
      r = 0
      j = @data_list.size
      while j > 0
        j -= 1
        data = @data_list[j]
        if (data.flags & ScanInfo::Flags::POP) != 0
          n_dif_ops += 1
          n_parens += 1
        else
          if (data.flags & ScanInfo::Flags::PUSH) != 0
            n_parens -= 1
            if n_parens == 0
              if r == 0
                if n_dif_ops > 0
                  if j > 0 and operator != Operator::AND_NOT
                    n_parens = 1
                    n_dif_ops = 0
                    r = j
                  else
                    new_data = ScanInfoData.new(start)
                    new_data.flags = ScanInfo::Flags::POP
                    new_data.logical_op = operator
                    @data_list << new_data
                    break
                  end
                else
                  data.flags &= ~ScanInfo::Flags::PUSH
                  data.logical_op = operator
                  break
                end
              else
                if n_dif_ops > 0
                  new_data = ScanInfoData.new(start)
                  new_data.flags = ScanInfo::Flags::POP
                  new_data.logical_op = operator
                  @data_list << new_data
                else
                  data.flags &= ~ScanInfo::Flags::PUSH
                  data.logical_op = operator
                  @data_list =
                    @data_list[0...j] +
                    @data_list[r..-1] +
                    @data_list[j...r]
                end
                break
              end
            end
          else
            if operator == Operator::AND_NOT or operator != data.logical_op
              n_dif_ops += 1
            end
          end
        end

        if j < 0
          raise ErrorMessage, "unmatched nesting level"
        end
      end
    end

    def build_not(context, code, i)
      last_data = @data_list.last
      return false if last_data.nil?

      case last_data.op
      when Operator::LESS
        last_data.op = Operator::GREATER_EQUAL
        last_data.end += 1
      when Operator::LESS_EQUAL
        last_data.op = Operator::GREATER
        last_data.end += 1
      when Operator::GREATER
        last_data.op = Operator::LESS_EQUAL
        last_data.end += 1
      when Operator::GREATER_EQUAL
        last_data.op = Operator::LESS
        last_data.end += 1
      when Operator::NOT_EQUAL
        last_data.op = Operator::EQUAL
        last_data.end += 1
      else
        if @data_list.size == 1
          if last_data.search_indexes.empty?
            if last_data.op == Operator::EQUAL
              last_data.op = Operator::NOT_EQUAL
              last_data.end += 1
            else
              return false
            end
          else
            last_data.logical_op = Operator::AND_NOT
            last_data.flags &= ~ScanInfo::Flags::PUSH
            @data_list.unshift(create_all_match_data)
          end
        else
          next_code = context.code
          return false if next_code.nil?

          case next_code.op
          when Operator::AND
            context.code_op = Operator::AND_NOT
          when Operator::AND_NOT
            context.code_op = Operator::AND
          when Operator::OR
            @data_list[-1, 0] = create_all_match_data
            put_logical_op(Operator::AND_NOT, i)
          else
            return false
          end
        end
      end

      true
    end

    def optimize
      optimized_data_list = []
      i = 0
      n = @data_list.size
      while i < n
        data = @data_list[i]
        next_data = @data_list[i + 1]
        i += 1
        if next_data.nil?
          optimized_data_list << data
          next
        end
        if range_operations?(data, next_data)
          between_data = create_between_data(data, next_data)
          optimized_data_list << between_data
          i += 1
          next
        end
        optimized_data_list << data
      end

      optimize_by_estimated_size(optimized_data_list)
    end

    def optimize_by_estimated_size(data_list)
      return data_list unless Groonga::ORDER_BY_ESTIMATED_SIZE

      start_index = nil
      data_list.size.times do |i|
        data = data_list[i]
        if data.logical_op != Operator::AND
          if start_index.nil?
            start_index = i
          else
            sort_by_estimated_size!(data_list, start_index...i)
            start_index = nil
          end
        end
      end
      unless start_index.nil?
        sort_by_estimated_size!(data_list, start_index...data_list.size)
      end
      data_list
    end

    def sort_by_estimated_size!(data_list, range)
      target_data_list = data_list[range]
      return if target_data_list.size < 2

      start_logical_op = target_data_list.first.logical_op
      sorted_data_list = target_data_list.sort_by do |data|
        estimator = ScanInfoDataSizeEstimator.new(data, @table)
        estimator.estimate
      end
      sorted_data_list.each do |sorted_data|
        sorted_data.logical_op = Operator::AND
      end
      sorted_data_list.first.logical_op = start_logical_op
      data_list[range] = sorted_data_list
    end

    def range_operations?(data, next_data)
      return false unless next_data.logical_op == Operator::AND

      op, next_op = data.op, next_data.op
      return false if !(lower_condition?(op) or lower_condition?(next_op))
      return false if !(upper_condition?(op) or upper_condition?(next_op))

      return false if data.args[0] != next_data.args[0]

      data_search_indexes = data.search_indexes
      return false if data_search_indexes.empty?

      data_search_indexes == next_data.search_indexes
    end

    def lower_condition?(operator)
      case operator
      when Operator::GREATER, Operator::GREATER_EQUAL
        true
      else
        false
      end
    end

    def upper_condition?(operator)
      case operator
      when Operator::LESS, Operator::LESS_EQUAL
        true
      else
        false
      end
    end

    def create_all_match_data
      data = ScanInfoData.new(0)
      data.end = 0
      data.flags = ScanInfo::Flags::PUSH
      data.op = Operator::CALL
      data.logical_op = Operator::OR
      data.args = [Context.instance["all_records"]]
      data.search_indexes = []
      data
    end

    def create_between_data(data, next_data)
      between_data = ScanInfoData.new(data.start)
      between_data.end = next_data.end + 1
      between_data.flags = data.flags
      between_data.op = Operator::CALL
      between_data.logical_op = data.logical_op
      between_data.args = create_between_data_args(data, next_data)
      between_data.search_indexes = data.search_indexes
      between_data
    end

    def create_between_data_args(data, next_data)
      between = Context.instance["between"]
      @expression.take_object(between)
      column = data.args[0]
      op, next_op = data.op, next_data.op
      if lower_condition?(op)
        min = data.args[1]
        min_operator = op
        max = next_data.args[1]
        max_operator = next_op
      else
        min = next_data.args[1]
        min_operator = next_op
        max = data.args[1]
        max_operator = op
      end
      if min_operator == Operator::GREATER
        min_border = "exclude"
      else
        min_border = "include"
      end
      if max_operator == Operator::LESS
        max_border = "exclude"
      else
        max_border = "include"
      end

      [
        between,
        column,
        min,
        @expression.allocate_constant(min_border),
        max,
        @expression.allocate_constant(max_border),
      ]
    end

    class BuildContext
      attr_accessor :status
      attr_reader :codes
      attr_reader :n_codes
      attr_reader :i
      attr_writer :code_op
      attr_accessor :data
      def initialize(expression)
        @expression = expression
        @status = :start
        @current_data = nil
        @codes = @expression.codes
        @n_codes = @codes.size
        @i = 0
        @code_op = nil
        @data = nil
      end

      def have_next?
        @i < @n_codes
      end

      def next
        @i += 1
        @code_op = nil
      end

      def code
        @codes[@i]
      end

      def code_op
        @code_op || code.op
      end
    end
  end
end
