module Groonga
  class QueryLogger
    def log(flag, mark, message)
      flag = Flag.find(flag) if flag.is_a?(Symbol)
      flag = flag.to_i if flag.is_a?(Flag)
      log_raw(flag, mark, message)
    end
  end
end
