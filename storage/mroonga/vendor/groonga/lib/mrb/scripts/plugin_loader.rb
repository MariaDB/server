module Groonga
  class PluginLoader
    class << self
      def load_file(path)
        begin
          load(path)
        rescue => error
          Context.instance.record_error(:plugin_error, error)
          nil
        end
      end
    end
  end
end
