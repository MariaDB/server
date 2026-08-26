# Standalone CMake project - NOT part of the main server build.
#
# Exercises MDEV-40608 (MariaDB-devel is incomplete for plugins): building
# server/client plugins against nothing but an installed/unpacked MariaDB
# package via find_package(mariadb-plugin) + MARIADB_ADD_PLUGIN(), the way
# an external plugin author would. See tests/mariadb-add-plugin-test/*.sh
# and *.ps1 to run this against an actual package (ZIP/tar.gz).

CMAKE_MINIMUM_REQUIRED(VERSION 3.16)
PROJECT(mariadb_add_plugin_test C CXX)

FIND_PACKAGE(mariadb-plugin REQUIRED)

MARIADB_ADD_PLUGIN(dummy_auth dummy_auth.c
  AUTHOR "MariaDB plc"
  DESCRIPTION "Dummy authentication plugin exercising MARIADB_ADD_PLUGIN"
  VERSION "1.0"
)

MARIADB_ADD_PLUGIN(dummy_client_auth dummy_client_auth.c
  CLIENT
  AUTHOR "MariaDB plc"
  DESCRIPTION "Dummy client authentication plugin exercising MARIADB_ADD_PLUGIN CLIENT"
  VERSION "1.0"
)

MARIADB_ADD_PLUGIN(dummy_storage_engine ha_dummy_se.cc
  STORAGE_ENGINE
  AUTHOR "MariaDB plc"
  DESCRIPTION "Dummy storage engine exercising MARIADB_ADD_PLUGIN STORAGE_ENGINE"
  VERSION "1.0"
)
