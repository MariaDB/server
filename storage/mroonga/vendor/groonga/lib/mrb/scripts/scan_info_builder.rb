module Groonga
  # TODO: Move me
  class ExpressionCode
    module Flags
      RELATIONAL_EXPRESSION = 0x01
    end
  end

  class ScanInfoBuilder
    module Status
      START = 0
      VAR = 1
      COL1 = 2
      COL2 = 3
      CONST = 4
    end

    def initialize(expression, operator, size)
      @data_list = []
      @expression = expression
      @operator = operator
      @size = size
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

      status = Status::START
      variable = @expression.get_var_by_offset(0)
      data = nil
      codes = @expression.codes
      n_codes = codes.size
      codes.each_with_index do |code, i|
        case code.op
        when *RELATION_OPERATORS
          status = Status::START
          data.op = code.op
          data.end = i
          data.match_resolve_index
          @data_list << data
          data = nil
        when *LOGICAL_OPERATORS
          put_logical_op(code.op, i)
          # TODO: rescue and return nil
          status = Status::START
        when Operator::PUSH
          data ||= ScanInfoData.new(i)
          if code.value == variable
            status = Status::VAR
          else
            data.args << code.value
            if status == Status::START
              data.flags |= ScanInfo::Flags::PRE_CONST
            end
            status = Status::CONST
          end
        when Operator::GET_VALUE
          case status
          when Status::START
            data ||= ScanInfoData.new(i)
            status = Status::COL1
            data.args << code.value
          when Status::CONST, Status::VAR
            status = Status::COL1
            data.args << code.value
          when Status::COL1
            raise ErrorMessage, "invalid expression: can't use column as a value: <#{code.value.name}>: <#{@expression.grn_inspect}>"
            status = Status::COL2
          when Status::COL2
            # Do nothing
          end
        when Operator::CALL
          data ||= ScanInfoData.new(i)
          if (code.flags & ExpressionCode::Flags::RELATIONAL_EXPRESSION) != 0 or
              (i + 1) == n_codes
            status = Status::START
            data.op = code.op
            data.end = i
            data.call_relational_resolve_indexes
            @data_list << data
            data = nil
          else
            status = Status::COL2
          end
        end
      end

      if @operator == Operator::OR and @size == 0
        first_data = @data_list.first
        if (first_data.flags & ScanInfo::Flags::PUSH) == 0 or
            first_data.logical_op != @operator
          raise ErrorMessage, "invalid expr"
        else
          first_data.flags &= ~ScanInfo::Flags::PUSH
          first_data.logical_op = @operator
        end
      else
        put_logical_op(@operator, n_codes)
      end

      optimize
    end

    private
    def valid?
      n_relation_expressions = 0
      n_logical_expressions = 0
      status = Status::START
      variable = @expression.get_var_by_offset(0)
      codes = @expression.codes
      codes.each do |code|
        case code.op
        when *RELATION_OPERATORS
          return false if status < Status::COL1
          return false if status > Status::CONST
          status = Status::START
          n_relation_expressions += 1
        when *ARITHMETIC_OPERATORS
          return false if status < Status::COL1
          return false if status > Status::CONST
          status = Status::START
          return false if n_relation_expressions != (n_logical_expressions + 1)
        when *LOGICAL_OPERATORS
          return false if status != Status::START
          n_logical_expressions += 1
          return false if n_logical_expressions >= n_relation_expressions
        when Operator::PUSH
          if code.value == variable
            status = Status::VAR
          else
            status = Status::CONST
          end
        when Operator::GET_VALUE
          case status
          when Status::START, Status::CONST, Status::VAR
            status = Status::COL1
          when Status::COL1
            status = Status::COL2
          when Status::COL2
            # Do nothing
          else
            return false
          end
        when Operator::CALL
          if (code.flags & ExpressionCode::Flags::RELATIONAL_EXPRESSION) != 0 or
              code == codes.last
            status = Status::START
            n_relation_expressions += 1
          else
            status = Status::COL2
          end
        else
          return false
        end
      end

      return false if status != Status::START
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
      optimized_data_list
    end

    def range_operations?(data, next_data)
      return false unless next_data.logical_op == Operator::AND

      op, next_op = data.op, next_data.op
      return false if !(lower_condition?(op) or lower_condition?(next_op))
      return false if !(upper_condition?(op) or upper_condition?(next_op))

      return false if data.args[0] != next_data.args[0]

      data_indexes = data.indexes
      return false if data_indexes.empty?

      data_indexes == next_data.indexes
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

    def create_between_data(data, next_data)
      between_data = ScanInfoData.new(data.start)
      between_data.end = next_data.end + 1
      between_data.flags = data.flags
      between_data.op = Operator::CALL
      between_data.logical_op = data.logical_op
      between_data.args = create_between_data_args(data, next_data)
      between_data.indexes = data.indexes
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
  end
end
