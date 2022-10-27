module Groonga
  class EvalContext
    def eval(script)
      proc = compile(script)
      instance_eval(&proc)
    end

    def method_missing(id, *args, &block)
      return super unless args.empty?
      return super if block_given?

      object = Context.instance[id.to_s]
      return super if object.nil?

      object
    end
  end
end
