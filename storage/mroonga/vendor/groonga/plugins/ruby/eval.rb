module Groonga
  module Ruby
    class EvalCommand < Command
      register("ruby_eval",
               [
                 "script",
               ])

      def run_body(input)
        script = input[:script]
        unless script.is_a?(String)
          message = "script must be a string: <#{script.inspect}>"
          raise Groonga::InvalidArgument, message
        end

        eval_context = EvalContext.new
        begin
          result = eval_context.eval(script)
        rescue Exception => error
          writer.map("result", 1) do
            writer.write("exception")
            writer.map("exception", 1) do
              writer.write("message")
              writer.write(error.message)
            end
          end
        else
          writer.map("result", 1) do
            writer.write("value")
            writer.write(result)
          end
        end
      end
    end
  end
end
