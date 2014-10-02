#!/bin/sh

list_paths()
{
    variable_name=$1
    echo "$variable_name = \\"
    sort | \
    sed \
      -e 's,^,\t,' \
      -e 's,$, \\,'
    echo "	\$(NULL)"
    echo
}

# image files.
ls *.svg *.png | \
    list_paths "image_files"
