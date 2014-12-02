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
  end
end
