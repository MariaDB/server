#!/usr/bin/perl

# Copyright (c) 2024, Väinö Mäkelä
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU Library General Public
# License as published by the Free Software Foundation; version 2
# of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Library General Public License for more details.
#
# You should have received a copy of the GNU Library General Public
# License along with this library; if not, write to the Free
# Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
# MA 02110-1335  USA

# This script generates a C header containing the supported mariadbd options,
# parsed from mariadbd help output.
#
# Usage:
#
# generate_option_list.pl > mariadbd_options.h

use strict;
use warnings;

sub split_comma {
    return split(/[\s,\.]+/, $_[0] =~ s/\([^\)]*\)//gr);
}

sub generate_typelibs {
    my %map = %{$_[0]};
    for my $option (sort (keys %map)) {
        print "\nstatic const char *valid_" . $option . "_values[] = {\n";
        for my $value (@{$map{$option}}) {
            print '"' . $value . "\",\n" if $value;
        }
        print "0\n};\n";
        print "static TYPELIB valid_" . $option . "_values_typelib = {\n"
            . "array_elements(valid_" . $option . "_values)-1,\n"
            . "\"\", valid_" . $option . "_values, 0};\n";
    }
}

sub generate_typelib_map {
    my $name = $_[0];
    my %map = %{$_[1]};
    my @options = sort (keys %map);
    print "\nstatic const char *mariadbd_${name}_options[] = {\n";
    for my $option (@options) {
        print '"' . $option . "\",\n";
    }
    print "};\n";
    print "\nstatic TYPELIB *mariadbd_${name}_typelibs[] = {\n";
    for my $option (@options) {
        print "&valid_" . $option . "_values_typelib,\n";
    }
    print "};\n";
}

my %enums;
my %sets;
my $help_output = `mariadbd --no-defaults --verbose --help`;

print "#include <my_global.h>\n";
print "static const char *mariadbd_valid_options[]= {\n";
while ($help_output =~ /
                       # Consider all lines that start with "  --" as options.
                       ^\ \ --([^\ =\[]+)
                       (?:
                           # Check for possible enum or set values until we hit
                           # "  -" at the start of a line. This won't work for
                           # the last option but should work for most ones.
                           (?:(?<!^\ \ -).)*
                           (?:(?:One\s+of:(.*?))|(?:Any\s+combination\s+of:(.*?)))
                           # Sets end with "  Use 'ALL'..."
                           (?=^\ \ (?:-|Use))
                       )?/xgms) {
    my $enum_part = $2;
    my $set_part = $3;
    my $option = $1 =~ s/-/_/gr;
    print '"' . $option . "\",\n";
    if ($enum_part) {
        my @cleaned = split_comma($enum_part);
        $enums{$option} = \@cleaned;
    }
    if ($set_part) {
        my @cleaned = split_comma($set_part);
        $sets{$option} = \@cleaned;
    }
}
print "};\n";

generate_typelibs(\%enums);
generate_typelib_map("enum", \%enums);

generate_typelibs(\%sets);
generate_typelib_map("set", \%sets);
