module Groonga
  module Sharding
    class LogicalTableRemoveCommand < Command
      register("logical_table_remove",
               [
                 "logical_table",
                 "shard_key",
                 "min",
                 "min_border",
                 "max",
                 "max_border",
                 "dependent",
                 "force",
               ])

      def run_body(input)
        @dependent = (input[:dependent] == "yes")
        @force = (input[:force] == "yes")

        enumerator = LogicalEnumerator.new("logical_table_remove", input)

        success = true
        enumerator.each do |shard, shard_range|
          remove_shard(shard, shard_range, enumerator.target_range)
        end
        writer.write(success)
      end

      private
      def remove_shard(shard, shard_range, target_range)
        cover_type = target_range.cover_type(shard_range)
        return if cover_type == :none

        shard_key = shard.key
        if shard_key.nil?
          if @force
            context.clear_error
          else
            message =
              "[logical_table_remove] shard_key doesn't exist: " +
              "<#{shard.key_name}>"
            raise InvalidArgument, message
          end
        end
        table = shard.table

        if cover_type == :all or ((table.nil? or shard_key.nil?) and @force)
          remove_table(shard, table)
          return
        end

        expression_builder = RangeExpressionBuilder.new(shard_key,
                                                        target_range)
        case cover_type
        when :partial_min
          remove_records(table) do |expression|
            expression_builder.build_partial_min(expression)
          end
          remove_table(shard, table) if table.empty?
        when :partial_max
          remove_records(table) do |expression|
            expression_builder.build_partial_max(expression)
          end
          remove_table(shard, table) if table.empty?
        when :partial_min_and_max
          remove_records(table) do |expression|
            expression_builder.build_partial_min_and_max(expression)
          end
          remove_table(shard, table) if table.empty?
        end
      end

      def collect_referenced_table_ids_from_index_ids(index_ids,
                                                      referenced_table_ids)
        database = context.database
        index_ids.each do |index_id|
          index = context[index_id]
          if index.nil?
            context.clear_error
            index_name = database[index_id]
            lexicon_name = index_name.split(".", 2)[0]
            lexicon_id = database[lexicon_name]
            referenced_table_ids << lexicon_id if lexicon_id
          else
            referenced_table_ids << index.domain_id
          end
        end
      end

      def collect_referenced_table_ids_from_column_name(column_name,
                                                        referenced_table_ids)
        database = context.database
        column_id = database[column_name]
        database.each_raw do |id, cursor|
          next if ID.builtin?(id)
          next if id == column_id

          context.open_temporary(id) do |object|
            if object.nil?
              context.clear_error
              next
            end

            case object
            when IndexColumn
              if object.source_ids.include?(column_id)
                collect_referenced_table_ids_from_index_ids([id],
                                                            referenced_table_ids)
              end
            end
          end
        end
      end

      def collect_referenced_table_ids_from_column(column,
                                                   referenced_table_ids)
        range = column.range
        case range
        when nil
          context.clear_error
        when Table
          referenced_table_ids << range.id
          collect_referenced_table_ids_from_index_ids(range.index_ids,
                                                      referenced_table_ids)
        end
        collect_referenced_table_ids_from_index_ids(column.index_ids,
                                                    referenced_table_ids)
      end

      def collect_referenced_table_ids_from_column_names(column_names)
        referenced_table_ids = []
        column_names.each do |column_name|
          column = context[column_name]
          if column.nil?
            context.clear_error
            collect_referenced_table_ids_from_column_name(column_name,
                                                          referenced_table_ids)
          else
            collect_referenced_table_ids_from_column(column,
                                                     referenced_table_ids)
          end
        end
        referenced_table_ids
      end

      def collect_referenced_table_ids(shard, table)
        return [] unless @dependent

        column_names = nil
        if table
          begin
            column_names = table.columns.collect(&:name)
          rescue
            context.clear_error
          end
        end
        if column_names.nil?
          prefix = "#{shard.table_name}."
          column_names = []
          context.database.each_name(:prefix => prefix) do |column_name|
            column_names << column_name
          end
        end

        collect_referenced_table_ids_from_column_names(column_names)
      end

      def remove_table(shard, table)
        if table.nil?
          unless @force
            if context.rc == Context::RC::SUCCESS.to_i
              error_class = InvalidArgument
            else
              rc = Context::RC.find(context.rc)
              error_class = rc.error_class
            end
            message = "[logical_table_remove] table is broken: " +
                      "<#{shard.table_name}>: #{context.error_message}"
            raise error_class, message
          end
          context.clear_error
        end

        referenced_table_ids = collect_referenced_table_ids(shard, table)

        if table.nil?
          remove_table_force(shard.table_name)
        else
          options = {:dependent => @dependent}
          if @force
            begin
              table.remove(options)
            rescue
              context.clear_error
              table.close
              remove_table_force(shard.table_name)
            end
          else
            table.remove(options)
          end
        end

        remove_referenced_tables(shard, referenced_table_ids)
      end

      def remove_table_force(table_name)
        database = context.database

        prefix = "#{table_name}."
        database.each_raw(:prefix => prefix) do |id, cursor|
          column = context[id]
          if column.nil?
            context.clear_error
            column_name = cursor.key
            remove_column_force(column_name)
            table = context[table_name]
            if table.nil?
              context.clear_error
            else
              table.close
            end
          else
            remove_column(column)
          end
        end

        table_id = database[table_name]
        return if table_id.nil?

        database.each_raw do |id, cursor|
          next if ID.builtin?(id)
          next if id == table_id

          context.open_temporary(id) do |object|
            if object.nil?
              context.clear_error
              next
            end

            case object
            when Table
              if object.domain_id == table_id
                begin
                  object.remove(:dependent => @dependent)
                rescue
                  context.clear_error
                  reference_table_name = object.name
                  object.close
                  remove_table_force(reference_table_name)
                end
              end
            when Column
              if object.range_id == table_id
                remove_column(object)
              end
            end
          end
        end

        Object.remove_force(table_name)
      end

      def remove_column(column)
        begin
          column.remove(:dependent => @dependent)
        rescue
          context.clear_error
          column_name = column.name
          column.close
          remove_column_force(column_name)
        end
      end

      def remove_column_force(column_name)
        database = context.database

        column_id = database[column_name]

        column = context[column_id]
        if column.nil?
          context.clear_error
        else
          column.index_ids.each do |id|
            index_column = context[id]
            if index_column.nil?
              context.clear_error
              index_column_name = database[id]
              remove_column_force(index_column_name)
            else
              remove_column(index_column)
            end
          end
          column.close
        end

        Object.remove_force(column_name)
      end

      def remove_referenced_tables(shard, referenced_table_ids)
        return if referenced_table_ids.empty?

        database = context.database
        shard_suffix = shard.range_data.to_suffix
        referenced_table_ids.uniq.each do |referenced_table_id|
          referenced_table_name = database[referenced_table_id]
          next if referenced_table_name.nil?
          next unless referenced_table_name.end_with?(shard_suffix)

          referenced_table = context[referenced_table_id]
          if referenced_table.nil?
            context.clear_error
            if @force
              Object.remove_force(referenced_table_name)
            end
            next
          end

          if @force
            begin
              referenced_table.remove(:dependent => @dependent)
            rescue
              context.clear_error
              referenced_table.close
              remove_table_force(referenced_table_name)
            end
          else
            referenced_table.remove(:dependent => @dependent)
          end
        end
      end

      def remove_records(table)
        expression = nil

        begin
          expression = Expression.create(table)
          yield(expression)
          table.delete(:expression => expression)
        ensure
          expression.close if expression
        end
      end
    end
  end
end
