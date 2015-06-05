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
      last_raw_entry = backtrace.last
      if last_raw_entry
        last_entry = BacktraceEntry.parse(last_raw_entry)
        file = last_entry.file
        line = last_entry.line
        method = last_entry.method
        # message = "#{file}:#{line}:#{method}: #{message}"
      else
        file = ""
        line = 0
        method = ""
      end
      log(log_level, file, line, method, message)

      backtrace.reverse_each.with_index do |raw_entry, i|
        next if i == 0
        entry = BacktraceEntry.parse(raw_entry)
        message = entry.message
        message = raw_entry if message.empty?
        log(log_level, entry.file, entry.line, entry.method, raw_entry)
      end
    end
  end
end
