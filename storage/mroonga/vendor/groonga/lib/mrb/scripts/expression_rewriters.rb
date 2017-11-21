module Groonga
  module ExpressionRewriters
    @rewriters = {}

    class << self
      def register(name, rewriter_class)
        @rewriters[name] = rewriter_class
      end

      def enabled?
        rewriters_table_name =
          Config["expression_rewriter.table"] || "expression_rewriters"
        rewriters_table = Context.instance[rewriters_table_name]
        return false if rewriters_table.nil?
        return false if rewriters_table.empty?

        true
      end

      def classes
        rewriters_table_name =
          Config["expression_rewriter.table"] || "expression_rewriters"
        rewriters_table = Context.instance[rewriters_table_name]
        return [] if rewriters_table.nil?

        rewriters_table.collect do |id|
          record = Record.new(rewriters_table, id)
          name = record.key
          rewriter = @rewriters[name]
          if rewriter.nil?
            plugin_name = record.plugin_name.value
            require plugin_name
            rewriter = @rewriters[name]
            raise "unknown rewriter: <#{name}>:<#{plugin_name}>" if rewriter.nil?
          end
          rewriter
        end
      end
    end
  end
end
