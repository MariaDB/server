cmake_policy(SET CMP0011 NEW)
cmake_policy(SET CMP0057 NEW)

# Add a component to component-group based install (compare to cmake_add_component)
macro(add_component compname)
  if("${compname}" IN_LIST CPACK_COMPONENTS_ALL)
    list(APPEND COMPONENTS_INSTALL ${compname})
    string(TOUPPER ${compname} _CPACK_ADDCOMP_UNAME)
    cmake_parse_arguments(CPACK_COMPONENT_${_CPACK_ADDCOMP_UNAME}
      "HIDDEN"
      "DISPLAY_NAME;DESCRIPTION;GROUP"
      ""
      ${ARGN}
    )
    string(TOUPPER ${CPACK_COMPONENT_${_CPACK_ADDCOMP_UNAME}_GROUP} _UGROUP)
    if("${CPACK_COMPONENT_GROUP_${_UGROUP}_DISPLAY_NAME}" STREQUAL "")
      message(FATAL_ERROR "group not found for ${compname}")
    endif()
  else()
    message(STATUS "add_component : ignoring ${compname}, not in CPACK_COMPONENTS_ALL")
  endif()
endmacro()

# Add a group to component-group based install (compare to cmake_add_component_group)
macro(add_component_group grpname)
  string(TOUPPER ${grpname} _CPACK_ADDGRP_UNAME)
  cmake_parse_arguments(CPACK_COMPONENT_GROUP_${_CPACK_ADDGRP_UNAME}
    "EXPANDED;HIDDEN"
    "DISPLAY_NAME;DESCRIPTION"
    ""
    ${ARGN}
    )
endmacro()

message(STATUS "CPACK_COMPONENTS_ALL=${CPACK_COMPONENTS_ALL}")
add_component_group(AlwaysInstall HIDDEN
  DISPLAY_NAME "AlwaysInstall"
  DESCRIPTION "Always installed components")

foreach(c Readme Common VCCRT)
  add_component(${c} GROUP AlwaysInstall)
endforeach()

add_component_group(MySQLServer EXPANDED DISPLAY_NAME "MariaDB Server" DESCRIPTION "Install server")
add_component(Server
  GROUP MySQLServer HIDDEN)
add_component(Client
  GROUP MySQLServer DISPLAY_NAME "Client Programs"
  DESCRIPTION "Various helpful (commandline) tools including the command line client")
add_component(Backup
  GROUP MySQLServer
  DISPLAY_NAME "Backup utilities"
  DESCRIPTION "Installs backup utilities(mariabackup and mbstream)")
 
#Miscellaneous hidden components, part of server / or client programs
foreach(comp connect_engine connect-engine-jdbc ClientPlugins aws-key-management rocksdb-engine)
  add_component(${comp} GROUP MySQLServer HIDDEN)
endforeach()

add_component_group(Devel
   DISPLAY_NAME "Development components"
   DESCRIPTION "Installs C/C++ header files and libraries")
add_component(Development
   GROUP Devel HIDDEN)
add_component(SharedLibraries
   GROUP Devel
   DISPLAY_NAME "Client C API library (shared)"
   DESCRIPTION "Installs shared client library")

include(${CMAKE_CURRENT_LIST_DIR}/ComponentsIgnore.cmake)
set(KNOWN_COMPONENTS ${COMPONENTS_IGNORE} ${COMPONENTS_INSTALL})
foreach(c ${CPACK_COMPONENTS_ALL})
  if(NOT (${c} IN_LIST KNOWN_COMPONENTS))
    message(FATAL_ERROR "Component ${c} is not known. Either install it using add_component() macro \
     or add to COMPONENTS_IGNORE list")
  endif()
endforeach()

set(CPACK_COMPONENTS_ALL ${COMPONENTS_INSTALL})

# Extra things beyond CMake components
# DBInstance (running mysql_install_db.exe)
set(WIX_FEATURE_MySQLServer_EXTRA_FEATURES "DBInstance;SharedClientServerComponents")

# Firewall exception for mysqld.exe
set(bin.mysqld.exe.FILE_EXTRA "
  <FirewallException Id='firewallexception.mysqld.exe' Name='[ProductName]' Scope='any'
       IgnoreFailure='yes' xmlns='http://schemas.microsoft.com/wix/FirewallExtension' 
  />
")

