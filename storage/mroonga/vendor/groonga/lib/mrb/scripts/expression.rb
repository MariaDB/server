require "scan_info"
require "scan_info_builder"

require "expression_size_estimator"

module Groonga
  class Expression
    def build_scan_info(op, size)
      begin
        builder = ScanInfoBuilder.new(self, op, size)
        builder.build
      rescue => error
        Context.instance.record_error(:invalid_argument, error)
        nil
      end
    end

    def estimate_size(table)
      begin
        estimator = ExpressionSizeEstimator.new(self, table)
        estimator.estimate
      rescue GroongaError => groonga_error
        context.set_groonga_error(groonga_error)
        table.size
      rescue => error
        context.record_error(:unknown_error, error)
        table.size
      end
    end

    private
    def context
      @context ||= Context.instance
    end
  end
end
