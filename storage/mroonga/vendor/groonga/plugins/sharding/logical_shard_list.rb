module Groonga
  module Sharding
    class LogicalShardListCommand < Command
      register("logical_shard_list",
               [
                 "logical_table",
               ])

      def run_body(input)
        enumerator = LogicalEnumerator.new("logical_shard_list",
                                           input,
                                           :require_shard_key => false)
        shard_names = enumerator.collect do |current_shard, shard_range|
          current_shard.table_name
        end

        writer.array("shards", shard_names.size) do
          shard_names.each do |shard_name|
            writer.map("shard", 1) do
              writer.write("name")
              writer.write(shard_name)
            end
          end
        end
      end
    end
  end
end
