module Groonga
  class Command
    @@classes = {}
    class << self
      def register_class(name, klass)
        @@classes[name] = klass
      end

      def find_class(name)
        @@classes[name]
      end
    end

    private
    def context
      @context ||= Context.instance
    end

    def writer
      @writer ||= context.writer
    end

    def query_logger
      @query_logger ||= context.query_logger
    end

    def cache_key(input)
      nil
    end

    def cache_output(key, options={})
      if key.nil?
        yield
      else
        cache = Cache.current
        cached_value = cache.fetch(key)
        if cached_value
          context.output = cached_value
          query_logger.log(:cache, ":", "cache(#{cached_value.bytesize})")
        else
          yield
          cache.update(key, context.output) if options[:update]
        end
      end
    end

    def run_internal(input)
      begin
        options = {
          :update => (input["cache"] != "no"),
        }
        cache_output(cache_key(input), options) do
          run_body(input)
        end
      rescue GroongaError => groonga_error
        context.set_groonga_error(groonga_error)
        nil
      rescue => error
        context.record_error(:command_error, error)
        nil
      end
    end
  end
end
