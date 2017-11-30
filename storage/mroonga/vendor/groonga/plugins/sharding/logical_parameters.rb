module Groonga
  module Sharding
    class LogicalParametersCommand < Command
      register("logical_parameters",
               [
                 "range_index",
               ])

      def run_body(input)
        range_index = parse_range_index(input[:range_index])

        parameters = [
          :range_index,
        ]
        writer.map("parameters", parameters.size) do
          parameters.each do |name|
            writer.write(name.to_s)
            writer.write(Parameters.__send__(name))
          end
        end

        Parameters.range_index = range_index if range_index
      end

      private
      def parse_range_index(value)
        case value
        when nil
          nil
        when "auto"
          :auto
        when "always"
          :always
        when "never"
          :never
        else
          message = "[logical_parameters][range_index] "
          message << "must be auto, always or never: <#{value}>"
          raise InvalidArgument, message
        end
      end
    end
  end
end
