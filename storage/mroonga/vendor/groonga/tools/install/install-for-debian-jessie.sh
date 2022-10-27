#!/bin/sh

set -e

sources_list_path=/etc/apt/sources.list.d/groonga.list

if [ ! -f $sources_list_path ]; then
    sudo cat <<SOURCES_LIST | sudo tee $sources_list_path
deb http://packages.groonga.org/debian/ jessie main
deb-src http://packages.groonga.org/debian/ jessie main
SOURCES_LIST
fi

sudo apt-get update
sudo apt-get install -y --allow-unauthenticated groonga-keyring
sudo apt-get update
sudo apt-get install -y -V groonga
