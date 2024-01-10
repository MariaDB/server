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
# generate_option_list.pl <mariadbd_path> <output_path>

use strict;
use warnings;

if ($#ARGV != 1) {
    print "usage: $0 <mariadbd_path> <output_path>\n";
    exit;
}
my $mariadbd_path = $ARGV[0];
my $output_path = $ARGV[1];

open(OUT_FH, '>', $output_path) or die $!;

sub split_comma {
    return split(/[\s,\.]+/, $_[0] =~ s/\([^\)]*\)//gr);
}

sub generate_typelibs {
    my %map = %{$_[0]};
    for my $option (sort (keys %map)) {
        print OUT_FH "\nstatic const char *valid_" . $option . "_values[] = {\n";
        for my $value (@{$map{$option}}) {
            print OUT_FH '"' . $value . "\",\n" if $value;
        }
        print OUT_FH "0\n};\n";
        print OUT_FH "static TYPELIB valid_" . $option . "_values_typelib = {\n"
            . "array_elements(valid_" . $option . "_values)-1,\n"
            . "\"\", valid_" . $option . "_values, 0};\n";
    }
}

sub generate_typelib_map {
    my $name = $_[0];
    my %map = %{$_[1]};
    my @options = sort (keys %map);
    print OUT_FH "\nstatic const char *mariadbd_${name}_options[] = {\n";
    for my $option (@options) {
        print OUT_FH '"' . $option . "\",\n";
    }
    print OUT_FH "};\n";
    print OUT_FH "\nstatic TYPELIB *mariadbd_${name}_typelibs[] = {\n";
    for my $option (@options) {
        print OUT_FH "&valid_" . $option . "_values_typelib,\n";
    }
    print OUT_FH "};\n";
}

my %enums;
my %sets;
my $help_output = readpipe('"' . $mariadbd_path =~ s/"/\\"/gr . '"'
                           . ' --no-defaults'
                           . ' --plugin-maturity=unknown'
                           . ' --plugin-load="'
                           . 'adt_null;'
                           . 'auth_0x0100;'
                           . 'auth_ed25519;'
                           . 'auth_gssapi;'
                           . 'auth_pam;'
                           # . 'auth_pam_v1;'
                           . 'auth_test_plugin;'
                           . 'cracklib_password_check;'
                           . 'debug_key_management;'
                           . 'dialog_examples;'
                           . 'disks;'
                           . 'example_key_management;'
                           . 'file_key_management;'
                           . 'func_test;'
                           . 'ha_archive;'
                           . 'ha_blackhole;'
                           . 'ha_connect;'
                           . 'ha_federatedx;'
                           . 'ha_mroonga;'
                           . 'handlersocket;'
                           # . 'ha_oqgraph;'
                           . 'ha_rocksdb;'
                           . 'ha_s3;'
                           . 'hashicorp_key_management;'
                           . 'ha_sphinx;'
                           . 'ha_spider;'
                           . 'ha_test_sql_discovery;'
                           . 'libdaemon_example;'
                           . 'locales;'
                           . 'metadata_lock_info;'
                           . 'mypluglib;'
                           . 'password_reuse_check;'
                           . 'provider_bzip2;'
                           . 'provider_lz4;'
                           . 'provider_lzma;'
                           . 'qa_auth_interface;'
                           . 'qa_auth_server;'
                           . 'query_cache_info;'
                           . 'query_response_time;'
                           . 'server_audit;'
                           . 'simple_password_check;'
                           . 'sql_errlog;'
                           . 'test_sql_service;'
                           . 'test_versioning;'
                           . 'type_mysql_json;'
                           . 'type_mysql_timestamp;'
                           . 'type_test;'
                           . 'wsrep_info"'
                           . ' --verbose'
                           . ' --help'
);

print OUT_FH "#ifndef _mariadbd_options_h\n";
print OUT_FH "#define _mariadbd_options_h\n";
print OUT_FH "#include <my_global.h>\n";
print OUT_FH "static const char *mariadbd_valid_options[]= {\n";
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
    print OUT_FH '"' . $option . "\",\n";
    if ($enum_part) {
        my @cleaned = split_comma($enum_part);
        $enums{$option} = \@cleaned;
    }
    if ($set_part) {
        my @cleaned = split_comma($set_part);
        $sets{$option} = \@cleaned;
    }
}
print OUT_FH "};\n";

generate_typelibs(\%enums);
generate_typelib_map("enum", \%enums);

generate_typelibs(\%sets);
generate_typelib_map("set", \%sets);
print OUT_FH "#endif /* _mariadbd_options_h */\n";

close(OUT_FH);
