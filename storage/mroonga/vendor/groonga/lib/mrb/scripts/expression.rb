require "expression_rewriter"
require "expression_rewriters"

require "expression_tree_builder"

require "scan_info"
require "scan_info_builder"

require "scan_info_data_size_estimator"
require "expression_size_estimator"

module Groonga
  class Expression
    def rewrite
      rewritten = nil
      begin
        return nil unless ExpressionRewriters.enabled?
        source = self
        ExpressionRewriters.classes.each do |rewriter_class|
          rewriter = rewriter_class.new(source)
          new_rewritten = rewriter.rewrite
          if new_rewritten
            rewritten.close if rewritten
            rewritten = new_rewritten
            source = rewritten
          end
        end
      rescue GroongaError => groonga_error
        context.set_groonga_error(groonga_error)
        rewritten.close if rewritten
        rewritten = nil
      rescue => error
        context.record_error(:invalid_argument, error)
        rewritten.close if rewritten
        rewritten = nil
      end
      rewritten
    end

    def build_scan_info(op, record_exist)
      begin
        builder = ScanInfoBuilder.new(self, op, record_exist)
        builder.build
      rescue => error
        context.record_error(:invalid_argument, error)
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
