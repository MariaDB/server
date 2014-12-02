module Groonga
  class Context
    def guard(fallback=nil)
      begin
        yield
      rescue => error
        logger.log_error(error)
        fallback
      end
    end

    def logger
      @logger ||= Logger.new
    end

    def record_error(rc, error)
      rc = RC.find(rc) if rc.is_a?(Symbol)
      self.rc = rc.to_i
      self.error_level = ErrorLevel.find(:error).to_i

      backtrace = error.backtrace
      entry = BacktraceEntry.parse(backtrace.first)
      self.error_file = entry.file
      self.error_line = entry.line
      self.error_method = entry.method
      self.error_message = error.message

      logger.log_error(error)
    end
  end
end
