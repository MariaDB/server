#!/usr/bin/env ruby

require "English"
require "nkf"
require "json"

print(<<HEADER.chomp)
column_create item_dictionary edict_desc COLUMN_SCALAR ShortText
column_create bigram item_dictionary_edict_desc COLUMN_INDEX|WITH_POSITION item_dictionary edict_desc
load --table item_dictionary
[
["_key","edict_desc","kana"]
HEADER

loop do
  raw_line = gets
  break if raw_line.nil?

  line = raw_line.encode("UTF-8", "EUC-JP")
  key, body = line.strip.split("/", 2)
  key = key.strip
  if /\s*\[(.+)\]\z/ =~ key
    key = $PREMATCH
    reading = $1
    body = "[#{reading}] #{body}"
    kana = NKF.nkf("-Ww --katakana", reading)
  else
    kana = NKF.nkf("-Ww --katakana", key)
  end
  puts(",")
  puts([key, body, kana].to_json)
end
puts
puts("]")
