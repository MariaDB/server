#!/usr/bin/env ruby
# -*- coding: utf-8 -*-

$KCODE = 'u'

require 'English'
require 'kconv'

class String
  def to_json
    a = split(//).map {|char|
      case char
      when '"' then '\\"'
      when '\\' then '\\\\'
      when "\b" then '\b'
      when "\f" then '\f'
      when "\n" then '\n'
      when "\r" then ''
      when "\t" then '\t'
      else char
      end
    }
    "\"#{a.join('')}\""
  end
end

class Array
  def to_json
    '[' + map {|element|
      element.to_json
    }.join(',') + ']'
  end
end

puts <<END
column_create item_dictionary edict_desc COLUMN_SCALAR ShortText
column_create bigram item_dictionary_edict_desc COLUMN_INDEX|WITH_POSITION item_dictionary edict_desc
load --table item_dictionary
[["_key","edict_desc","kana"],
END

while !STDIN.eof?
  line = Kconv.toutf8(gets)
  key, body = line.strip.split('/', 2)
  key = key.strip
  if /\s*\[(.+)\]\z/ =~ key
    key = $PREMATCH
    reading = $1
    body = "[#{reading}] #{body}"
    kana = NKF.nkf("-Ww --katakana", reading)
  else
    kana = NKF.nkf("-Ww --katakana", key)
  end
  puts [key, body, kana].to_json
end
puts ']'
