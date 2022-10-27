#!/usr/bin/env ruby
#
# Copyright(C) 2016  Kouhei Sutou <kou@clear-code.com>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA

require "pp"
require "json"
require "groonga/command/parser"

if ARGV.empty?
  puts("Usage:")
  puts(" #{$0} schema.grn object_list_result.json")
  puts(" #{$0} schema.grn < object_list_result.json")
  puts(" groonga DB_PATH object_list | #{$0} schema.grn")
  exit(false)
end

schema_grn = ARGV.shift

schema = {}
Groonga::Command::Parser.parse(File.read(schema_grn)) do |type, *args|
  case type
  when :on_command
    command, = args
    case command.name
    when "table_create"
      if command.table_no_key?
        type = "table:no_key"
      elsif command.table_pat_key?
        type = "table:pat_key"
      elsif command.table_dat_key?
        type = "table:dat_key"
      else
        type = "table:hash_key"
      end
      name = command[:name]
      schema[name] ||= []
      schema[name] << {
        :type => type,
        :flags => command.flags.join("|"),
      }
    when "column_create"
      if command.column_index?
        type = "column:index"
      elsif command.column_vector? or command.type == "ShortText"
        type = "column:var_size"
      else
        type = "column:fix_size"
      end
      name = "#{command[:table]}.#{command[:name]}"
      schema[name] ||= []
      schema[name] << {
        :type => type,
        :flags => command.flags.join("|"),
      }
    end
  end
end

MAX_RESERVED_ID = 255
response = JSON.parse(ARGF.read)
body = response[1]
body.each do |name, object|
  id = object["id"]
  next if id <= MAX_RESERVED_ID
  normalized_name = name.gsub(/\d{4}\d{2}(?:\d{2})?/, "YYYYMMDD")

  definitions = schema[normalized_name]
  if definitions.nil?
    next if object["type"]["name"] == "proc"
    puts("Unknown table/column: #{name}(#{id})")
    exit(false)
  end

  type = object["type"]
  if type.nil?
    puts("[invalid][no-type] #{id}:#{name}")
    puts(PP.pp(object, "").gsub(/^/, "  "))
    next
  end

  type_name = type["name"]
  valid_type_names = definitions.collect {|definition| definition[:type]}
  unless valid_type_names.include?(type["name"])
    expected = "expected:[#{valid_type_names.join(", ")}]"
    puts("[invalid][wrong-type] #{id}:#{name} <#{type_name}> #{expected}")
    puts(PP.pp(object, "").gsub(/^/, "  "))
    puts(PP.pp(definitions, "").gsub(/^/, "  "))
    next
  end
end
