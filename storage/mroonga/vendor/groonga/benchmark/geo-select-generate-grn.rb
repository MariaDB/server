#!/usr/bin/env ruby
# -*- coding: utf-8 -*-

if ARGV.size != 2
  puts "Usage: #{$0} SOURCE_CSV OUTPUT_GRN"
  puts " e.g.: #{$0} fixtures/geo-select/13_2010.CSV fixtures/geo-select/load.grn"
  exit(false)
end

csv, grn = ARGV

require "fileutils"
require "csv"

FileUtils.mkdir_p(File.dirname(grn))
File.open(grn, "w") do |output|
  output.print(<<-EOH.strip)
table_create Addresses TABLE_HASH_KEY ShortText
column_create Addresses location COLUMN_SCALAR WGS84GeoPoint

table_create Locations TABLE_PAT_KEY WGS84GeoPoint
column_create Locations address COLUMN_INDEX Addresses location

load --table Addresses
[
["_key", "location"]
EOH

  headers = nil
  csv_foreach_args = [csv]
  csv_foreach_args << {:encoding => "UTF-8"} if defined?(Encoding)
  CSV.foreach(*csv_foreach_args) do |row|
    if headers.nil?
      headers = row
    else
      record = {}
      headers.each_with_index do |header, i|
        record[header] = row[i]
      end
      central_value_p = record["代表フラグ"] == "1"
      next unless central_value_p
      name =
        record["都道府県名"] + record["市区町村名"] +
        record["大字・町丁目"] + record["街区符号・地番"]
      location = "%sx%s" % [record["緯度"], record["経度"]]
      output.print(",\n[\"#{name}\", \"#{location}\"]")
    end
  end
  output.print(<<-EOF)

]
EOF
end
