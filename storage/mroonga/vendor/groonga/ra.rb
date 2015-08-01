#!/usr/bin/env ruby

puts "table_create X TABLE_NO_KEY"
puts "column_create X a COLUMN_SCALAR Int64"
puts "load --table X"
puts "["
n_records = 2 ** 28
(n_records - 1).times do |i|
  puts "{\"a\": #{i}},"
end
puts "{\"a\": #{n_records - 1}}"
puts "]"
