FIND_PACKAGE(CURL)
IF(NOT CURL_FOUND)
  # Can't build plugin
  MESSAGE_ONCE(WARNING "Hashicorp Key Management plugin requires curl development package")
  RETURN()
ENDIF()

SET_PACKAGE_PROPERTIES(CURL PROPERTIES TYPE REQUIRED)

INCLUDE_DIRECTORIES(${CURL_INCLUDE_DIRS})

set(CPACK_RPM_hashicorp-key-management_PACKAGE_SUMMARY "Hashicorp Key Management plugin for MariaDB" PARENT_SCOPE)
set(CPACK_RPM_hashicorp-key-management_PACKAGE_DESCRIPTION "This encryption plugin uses Hashicorp Vault for storing encryption
 keys for MariaDB Data-at-Rest encryption." PARENT_SCOPE)

MYSQL_ADD_PLUGIN(HASHICORP_KEY_MANAGEMENT
  hashicorp_key_management_plugin.cc
  LINK_LIBRARIES ${CURL_LIBRARIES}
  CONFIG hashicorp_key_management.cnf
  COMPONENT hashicorp-key-management
  MODULE_ONLY)

ADD_FEATURE_INFO(HASHICORP_KEY_MANAGEMENT "ON" "Hashicorp Key Management Plugin")

INSTALL_DOCUMENTATION(hashicorp_key_management.txt
  COMPONENT plugin-hashicorp-key-management)
