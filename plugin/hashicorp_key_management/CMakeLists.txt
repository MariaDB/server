INCLUDE(FindCURL)
IF(NOT CURL_FOUND)
  # Can't build plugin
  RETURN()
ENDIF()

INCLUDE_DIRECTORIES(${CURL_INCLUDE_DIR})

MYSQL_ADD_PLUGIN(HASHICORP_KEY_MANAGEMENT
  hashicorp_key_management_plugin.cc
  LINK_LIBRARIES ${CURL_LIBRARIES}
  CONFIG hashicorp_key_management.cnf
  COMPONENT hashicorp-key-management)

INSTALL_DOCUMENTATION(hashicorp_key_management.txt
        COMPONENT hashicorp-key-management)
