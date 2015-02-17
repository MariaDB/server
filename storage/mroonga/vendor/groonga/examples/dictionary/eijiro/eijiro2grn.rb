#!/usr/bin/env ruby
# -*- coding: utf-8 -*-

$KCODE = 'u'

require 'rubygems'
require 'fastercsv'

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
column_create item_dictionary eijiro_trans COLUMN_SCALAR ShortText
column_create item_dictionary eijiro_exp COLUMN_SCALAR ShortText
column_create item_dictionary eijiro_level COLUMN_SCALAR Int32
column_create item_dictionary eijiro_memory COLUMN_SCALAR Int32
column_create item_dictionary eijiro_modify COLUMN_SCALAR Int32
column_create item_dictionary eijiro_pron COLUMN_SCALAR ShortText
column_create item_dictionary eijiro_filelink COLUMN_SCALAR ShortText
column_create bigram item_dictionary_eijiro_trans COLUMN_INDEX|WITH_POSITION item_dictionary eijiro_trans
load --table item_dictionary
[["_key","norm","eijiro_trans","eijiro_exp","eijiro_level","eijiro_memory","eijiro_modify","eijiro_pron","eijiro_filelink","kana"],
END

n = 0
FasterCSV.new(ARGF, :row_sep => "\r\n").each {|l|
  if n > 0
    keyword,word,trans,exp,level,memory,modify,pron,filelink = l
    kana = ''
    if trans =~ /【＠】(.*?)(【|$)/
      kana = $1.split("、")
    end
    puts [word,keyword,trans,exp,level,memory,modify,pron,filelink,kana].map{|e| e || ''}.to_json
  end
  n += 1
}

puts "]"
