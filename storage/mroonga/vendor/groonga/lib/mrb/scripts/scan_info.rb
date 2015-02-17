module Groonga
  class ScanInfo
    module Flags
      ACCESSOR  = 0x01
      PUSH      = 0x02
      POP       = 0x04
      PRE_CONST = 0x08
    end

    def apply(data)
      self.op = data.op
      self.logical_op = data.logical_op
      self.end = data.end
      self.query = data.query
      self.flags = data.flags
      if data.max_interval
        self.max_interval = data.max_interval
      end
      if data.similarity_threshold
        self.similarity_threshold = data.similarity_threshold
      end
      if data.scorer
        self.scorer = data.scorer
      end
      data.args.each do |arg|
        push_arg(arg)
      end
      data.indexes.each do |index, section_id, weight|
        put_index(index, section_id, weight)
      end
    end
  end
end
