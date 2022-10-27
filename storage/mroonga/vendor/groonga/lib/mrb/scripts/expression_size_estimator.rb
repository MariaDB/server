module Groonga
  class ExpressionSizeEstimator
    def initialize(expression, table)
      @expression = expression
      @table = table
      @table_size = @table.size
    end

    def estimate
      builder = ScanInfoBuilder.new(@expression, Operator::OR, false)
      data_list = builder.build
      return @table_size if data_list.nil?

      current_size = 0
      sizes = []
      data_list.each do |data|
        if (data.flags & ScanInfo::Flags::POP) != 0
          size = sizes.pop
          case data.logical_op
          when Operator::AND, Operator::AND_NOT
            current_size = size if size < current_size
          when Operator::OR
            current_size = size if size > current_size
          else
            message = "invalid logical operator: <#{data.logical_op.inspect}>"
            raise InvalidArgument, message
          end
        else
          if (data.flags & ScanInfo::Flags::PUSH) != 0
            sizes.push(current_size)
            current_size = 0
          end

          estimator = ScanInfoDataSizeEstimator.new(data, @table)
          size = estimator.estimate
          case data.logical_op
          when Operator::AND
            current_size = size if size < current_size
          when Operator::AND_NOT
            size = @table_size - size
            size = 0 if size < 0
            current_size = size if size < current_size
          when Operator::OR
            current_size = size if size > current_size
          else
            message = "invalid logical operator: <#{data.logical_op.inspect}>"
            raise InvalidArgument, message
          end
        end
      end
      current_size
    end
  end
end
