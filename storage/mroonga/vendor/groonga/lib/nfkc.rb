#!/usr/bin/env ruby
# -*- coding: utf-8 -*-
#
# Copyright(C) 2010 Brazil
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License version 2.1 as published by the Free Software Foundation.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

$KCODE = 'u'

CUSTOM_RULE_PATH = 'nfkc-custom-rules.txt'

def gen_bc(file, hash, level)
  bl = ' ' * (level * 2)
  h2 = {}
  hash.each{|key,val|
    head = key[0]
    rest = key[1..-1]
    if h2[head]
      h2[head][rest] = val
    else
      h2[head] = {rest => val}
    end
  }
  if h2.size < 3
    h2.keys.sort.each{|k|
      if (0x80 < k)
        file.printf("#{bl}if (str[#{level}] < 0x%02X) { return #{$lv}; }\n", k)
      end
      h = h2[k]
      if h.keys.join =~ /^\x80*$/
        $lv, = h.values
      else
        file.printf("#{bl}if (str[#{level}] == 0x%02X) {\n", k)
        gen_bc(file, h, level + 1)
        file.puts bl + '}'
      end
    }
    file.puts bl + "return #{$lv};"
  else
    file.puts bl + "switch (str[#{level}]) {"
    lk = 0x80
    br = true
    h2.keys.sort.each{|k|
      if (lk < k)
        for j in lk..k-1
          file.printf("#{bl}case 0x%02X :\n", j)
        end
        br = false
      end
      unless br
        file.puts bl + "  return #{$lv};"
        file.puts bl + '  break;'
      end
      h = h2[k]
      file.printf("#{bl}case 0x%02X :\n", k)
      if h.keys.join =~ /^\x80*$/
        $lv, = h.values
        br = false
      else
        gen_bc(file, h, level + 1)
        file.puts bl + '  break;'
        br = true
      end
      lk = k + 1
    }
    file.puts bl + 'default :'
    file.puts bl + "  return #{$lv};"
    file.puts bl + '  break;'
    file.puts bl + '}'
  end
end

def generate_blockcode_char_type(file, option)
  bc = {}
  open("|./icudump --#{option}").each{|l|
    src,_,code = l.chomp.split("\t")
    str = src.split(':').collect{|c| format("%c", c.hex)}.join
    bc[str] = code
  }
  $lv = 0
  gen_bc(file, bc, 0)
end

def ccpush(hash, src, dst)
  head = src.shift
  hash[head] = {} unless hash[head]
  if head
    ccpush(hash[head], src, dst)
  else
    hash[head] = dst
  end
end

def subst(hash, str)
  cand = nil
  src = str.split(//)
  for i in 0..src.size-1
    h = hash
    for j in i..src.size-1
      head = src[j]
      h = h[head]
      break unless h
      if h[nil]
        cand = src[0,i].to_s + h[nil] + src[j + 1..-1].to_s
      end
    end
    return cand if cand
  end
  return str
end

def map_entry(map1, cc, src, dst)
  dst.downcase! unless $case_sensitive
  loop {
    dst2 = subst(cc, dst)
    break if dst2 == dst
    dst = dst2
  }
  unless $keep_space
    dst = $1 if dst =~ /^ +([^ ].*)$/
  end
  map1[src] = dst if src != dst
end

def create_map1()
  cc = {}
  open('|./icudump --cc').each{|l|
    _,src,dst = l.chomp.split("\t")
    if cc[src]
      STDERR.puts "caution: ambiguous mapping #{src}|#{cc[src]}|#{dst}" if cc[src] != dst
    end
    ccpush(cc, src.split(//), dst)
  }
  map1 = {}
  open('|./icudump --nfkd').each{|l|
    n,src,dst = l.chomp.split("\t")
    map_entry(map1, cc, src, dst)
  }
  if File.readable?(CUSTOM_RULE_PATH)
    open(CUSTOM_RULE_PATH).each{|l|
      src,dst = l.chomp.split("\t")
      map_entry(map1, cc, src, dst)
    }
  end
  unless $case_sensitive
    for c in 'A'..'Z'
      map1[c] = c.downcase
    end
  end
  return map1
end

def create_map2(map1)
  cc = {}
  open('|./icudump --cc').each{|l|
    _,src,dst = l.chomp.split("\t")
    src = src.split(//).collect{|c| map1[c] || c}.join
    dst = map1[dst] || dst
    if cc[src] && cc[src] != dst
      STDERR.puts("caution: inconsitent mapping '#{src}' => '#{cc[src]}'|'#{dst}'")
    end
    cc[src] = dst if src != dst
  }
  loop {
    noccur = 0
    cc2 = {}
    cc.each {|src,dst|
      src2 = src
      chars = src.split(//)
      l = chars.size - 1
      for i in 0..l
        for j in i..l
          next if i == 0 && j == l
          str = chars[i..j].join
          if map1[str]
            STDERR.printf("caution: recursive mapping '%s'=>'%s'\n", str, map1[str])
          end
          if cc[str]
            src2 = (i > 0 ? chars[0..i-1].join : '') + cc[str] + (j < l ? chars[j+1..l].join : '')
            noccur += 1
          end
        end
      end
      cc2[src2] = dst if src2 != dst
    }
    cc = cc2
    STDERR.puts("substituted #{noccur} patterns.")
    break if noccur == 0
    STDERR.puts('try again..')
  }
  return cc
end

def generate_map1(file, hash, level)
  bl = ' ' * ((level + 0) * 2)
  if hash['']
    dst = ''
    hash[''].each_byte{|b| dst << format('\x%02X', b)}
    file.puts "#{bl}return \"#{dst}\";"
    hash.delete('')
  end
  return if hash.empty?
  h2 = {}
  hash.each{|key,val|
    head = key[0]
    rest = key[1..-1]
    if h2[head]
      h2[head][rest] = val
    else
      h2[head] = {rest => val}
    end
  }
  if h2.size == 1
    h2.each{|key,val|
      file.printf("#{bl}if (str[#{level}] == 0x%02X) {\n", key)
      generate_map1(file, val, level + 1)
      file.puts bl + '}'
    }
  else
    file.puts "#{bl}switch (str[#{level}]) {"
    h2.keys.sort.each{|k|
      file.printf("#{bl}case 0x%02X :\n", k)
      generate_map1(file, h2[k], level + 1)
      file.puts("#{bl}  break;")
    }
    file.puts bl + '}'
  end
end

def gen_map2_sub2(file, hash, level, indent)
  bl = ' ' * ((level + indent + 0) * 2)
  if hash['']
    file.print "#{bl}return \""
    hash[''].each_byte{|b| file.printf('\x%02X', b)}
    file.puts "\";"
    hash.delete('')
  end
  return if hash.empty?

  h2 = {}
  hash.each{|key,val|
    head = key[0]
    rest = key[1..-1]
    if h2[head]
      h2[head][rest] = val
    else
      h2[head] = {rest => val}
    end
  }

  if h2.size == 1
    h2.each{|key,val|
      file.printf("#{bl}if (prefix[#{level}] == 0x%02X) {\n", key)
      gen_map2_sub2(file, val, level + 1, indent)
      file.puts bl + '}'
    }
  else
    file.puts "#{bl}switch (prefix[#{level}]) {"
    h2.keys.sort.each{|k|
      file.printf("#{bl}case 0x%02X :\n", k)
      gen_map2_sub2(file, h2[k], level + 1, indent)
      file.puts("#{bl}  break;")
    }
    file.puts bl + '}'
  end
end

def gen_map2_sub(file, hash, level)
  bl = ' ' * ((level + 0) * 2)
  if hash['']
    gen_map2_sub2(file, hash[''], 0, level)
    hash.delete('')
  end
  return if hash.empty?
  h2 = {}
  hash.each{|key,val|
    head = key[0]
    rest = key[1..-1]
    if h2[head]
      h2[head][rest] = val
    else
      h2[head] = {rest => val}
    end
  }
  if h2.size == 1
    h2.each{|key,val|
      file.printf("#{bl}if (suffix[#{level}] == 0x%02X) {\n", key)
      gen_map2_sub(file, val, level + 1)
      file.puts bl + '}'
    }
  else
    file.puts "#{bl}switch (suffix[#{level}]) {"
    h2.keys.sort.each{|k|
      file.printf("#{bl}case 0x%02X :\n", k)
      gen_map2_sub(file, h2[k], level + 1)
      file.puts("#{bl}  break;")
    }
    file.puts bl + '}'
  end
end

def generate_map2(file, map2)
  suffix = {}
  map2.each{|src,dst|
    chars = src.split(//)
    if chars.size != 2
      STDERR.puts "caution: more than two chars in pattern #{chars.join('|')}"
    end
    s = chars.pop
    if suffix[s]
      suffix[s][chars.join] = dst
    else
      suffix[s] = {chars.join=>dst}
    end
  }
  gen_map2_sub(file, suffix, 0)
end

template = <<END
/* -*- c-basic-offset: 2 -*- */
/* Copyright(C) 2010 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

don't edit this file by hand. it generated automatically by nfkc.rb
*/

#include "nfkc.h"

#ifdef GRN_WITH_NFKC

unsigned char
grn_nfkc_char_type(const unsigned char *str)
{
%  return -1;
}

const char *
grn_nfkc_map1(const unsigned char *str)
{
%  return 0;
}

const char *
grn_nfkc_map2(const unsigned char *prefix, const unsigned char *suffix)
{
%  return 0;
}

#endif /* GRN_WITH_NFKC */

END

######## main #######

ARGV.each{|arg|
  case arg
  when /-*c/i
    $case_sensitive = true
  when /-*s/i
    $keep_space = true
  end
}

STDERR.puts('compiling icudump')
system('cc -Wall -O3 -o icudump icudump.c -licui18n')

STDERR.puts('creating map1..')
map1 = create_map1()

STDERR.puts('creating map2..')
map2 = create_map2(map1)

outf = open('nfkc.c', 'w')

tmps = template.split(/%/)

#STDERR.puts('generating block code..')
#outf.print(tmps.shift)
#generate_blockcode_char_type(outf, 'bc')

STDERR.puts('generating char type code..')
outf.print(tmps.shift)
generate_blockcode_char_type(outf, 'gc')

STDERR.puts('generating map1 code..')
outf.print(tmps.shift)
generate_map1(outf, map1, 0)

STDERR.puts('generating map2 code..')
outf.print(tmps.shift)
generate_map2(outf, map2)

outf.print(tmps.shift)
outf.close
STDERR.puts('done.')
