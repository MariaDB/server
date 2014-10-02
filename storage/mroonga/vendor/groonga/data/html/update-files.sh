#!/bin/sh

list_paths()
{
    variable_name=$1
    echo "$variable_name = \\"
    sort | \
    sed \
      -e 's,^,\t,' \
      -e 's,$, \\,'
    echo "\t\$(NULL)"
    echo
}

find "admin" -type f | \
    sort | \
    list_paths "nobase_dist_html_DATA"
