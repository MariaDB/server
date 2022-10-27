#!/usr/bin/env ruby
# -*- coding: utf-8 -*-

require 'rexml/document'
require 'rexml/parsers/streamparser'
require 'rexml/parsers/baseparser'
require 'rexml/streamlistener'

#REXML::Document.new(STDIN)

class MyListener
  include REXML::StreamListener
  def tag_start(name, attrs)
    # p name, attrs
    case name
    when 'entry'
      @n = 0
    end
  end
  def tag_end name
    # p "tag_end: #{x}"
    case name
    when 'sense'
      @n += 1
    when 'entry'
      @n_ents += 1
      puts "#{@ent}:#{@n}" if (@n > 8)
    when 'ent_seq'
      @ent = @text
    end
  end

  def text(text)
    @text = text
  end

  def xmldecl(version, encoding, standalone)
    @n_ents = 0
  end
end

REXML::Parsers::StreamParser.new(STDIN, MyListener.new).parse
