module Groonga
  class Record
    attr_reader :table
    attr_reader :id

    def inspect
      super.gsub(/>\z/) do
        "@id=#{@id.inspect}, @table=#{@table.inspect}>"
      end
    end

    def [](name)
      column = find_column(name)
      if column.nil?
        raise InvalidArgument, "unknown column: <#{absolute_column_name(name)}>"
      end
      column[@id]
    end

    def method_missing(name, *args, &block)
      return super unless args.empty?

      column = find_column(name)
      return super if column.nil?

      column[@id]
    end

    private
    def absolute_column_name(name)
      "#{@table.name}.#{name}"
    end

    def find_column(name)
      Context.instance[absolute_column_name(name)]
    end
  end
end
