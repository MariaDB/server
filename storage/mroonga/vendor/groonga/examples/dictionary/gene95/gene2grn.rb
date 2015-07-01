#!/usr/bin/env ruby

require "json"

print(<<HEADER.chomp)
column_create item_dictionary gene95_desc COLUMN_SCALAR ShortText
column_create bigram item_dictionary_gene95_desc COLUMN_INDEX|WITH_POSITION item_dictionary gene95_desc
load --table item_dictionary
[
["_key","gene95_desc"]
HEADER

loop do
  raw_key = gets
  break if raw_key.nil?
  raw_body = gets

  key = nil
  body = nil
  begin
    key = raw_key.encode("UTF-8", "Windows-31J").strip
    body = raw_body.encode("UTF-8", "Windows-31J").strip
  rescue EncodingError
    $stderr.puts("Ignore:")
    $stderr.puts("   key: <#{raw_key}>")
    $stderr.puts("  body: <#{raw_body}>")
    next
  end
  puts(",")
  print([key, body].to_json)
end
puts
puts("]")
