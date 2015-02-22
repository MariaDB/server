module Groonga
  class Logger
    def log_error(error)
      log_level = Level::ERROR.to_i

      if error.is_a?(Error)
        message = error.message
      else
        message = "#{error.class}: #{error.message}"
      end
      backtrace = error.backtrace
      first_raw_entry = backtrace.first
      if first_raw_entry
        first_entry = BacktraceEntry.parse(first_raw_entry)
        file = first_entry.file
        line = first_entry.line
        method = first_entry.method
      else
        file = ""
        line = 0
        method = ""
      end
      log(log_level, file, line, method, message)

      backtrace.each do |raw_entry|
        entry = BacktraceEntry.parse(raw_entry)
        log(log_level, entry.file, entry.line, entry.method, raw_entry)
      end
    end
  end
end
