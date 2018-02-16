module Groonga
  class CommandInput
    include Enumerable

    def each
      args = arguments
      arg = Record.new(args, nil)
      args.each do |id|
        arg.id = id
        key = arg.key
        yield(key, self[key])
      end
    end
  end
end
