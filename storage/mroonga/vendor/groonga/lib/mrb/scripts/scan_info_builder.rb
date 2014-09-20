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
            raise "invalid expression: can't use column as a value: <#{code.value.name}>: <#{@expression.grn_inspect}>"
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
          raise "invalid expr"
        else
          first_data.flags &= ~ScanInfo::Flags::PUSH
          first_data.logical_op = @operator
        end
      else
        put_logical_op(@operator, n_codes)
      end

      @data_list
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
                  end
                else
                  data.flags &= ~ScanInfo::Flags::PUSH
                  data.logical_op = operator
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
              end
            end
          else
            if operator == Operator::AND_NOT or operator != data.logical_op
              n_dif_ops += 1
            end
          end
        end

        if j < 0
          raise GRN_INVALID_ARGUMENT.new("unmatched nesting level")
        end
      end
    end
  end
end
