

# TODO: just put this in cargo.toml and parse it
# CMAKE_PARSE_ARGUMENTS(ARG
# "STORAGE_ENGINE;STATIC_ONLY;MODULE_ONLY;MANDATORY;DEFAULT;DISABLED;NOT_EMBEDDED;RECOMPILE_FOR_EMBEDDED;CLIENT"
# "MODULE_OUTPUT_NAME;STATIC_OUTPUT_NAME;COMPONENT;CONFIG;VERSION"
# "LINK_LIBRARIES;DEPENDS"
# ${ARGN}
# )

macro(CONFIGURE_RUST_PLUGINS)
  set(rust_dir "${CMAKE_SOURCE_DIR}/rust")
  set(cargo_target_dir "${CMAKE_CURRENT_BINARY_DIR}/rust_target")
  message("rust dir ${rust_dir}")

  execute_process(COMMAND python3 "${rust_dir}/cmake_helper.py" OUTPUT_VARIABLE plugins)
  message("plugins: ${plugins}")

  # See cmake_helper.py for the output that we get here. We loop through each
  # plugin
  foreach(entry IN LISTS plugins)
    string(REPLACE "|" ";" entry "${entry}")
    list(GET entry 0 env_name)
    list(GET entry 1 target_name)
    list(GET entry 2 cargo_name)
    list(GET entry 3 staticlib_name)
    list(GET entry 4 dynlib_name)

    # Copied from plugin.cmake, set default `howtobuild`
    if(ARG_DISABLED)
      set(howtobuild NO)
    elseif(compat STREQUAL ".")
      set(howtobuild DYNAMIC)
    elseif(compat STREQUAL "with.")
      if(NOT ARG_MODULE_ONLY)
        set(howtobuild STATIC)
      ELSE()
        set(howtobuild DYNAMIC)
      endif()
    elseif(compat STREQUAL ".without")
      set(howtobuild NO)
    elseif(compat STREQUAL "with.without")
      set(howtobuild STATIC)
    else()
      set(howtobuild DYNAMIC)
    endif()


    # NO - not at all
    # YES - static if possible, otherwise dynamic if possible, otherwise abort
    # AUTO - static if possible, otherwise dynamic, if possible
    # STATIC - static if possible, otherwise not at all
    # DYNAMIC - dynamic if possible, otherwise not at all
    set(${env_name} ${howtobuild} CACHE STRING
      "How to build plugin ${cargo_name}. Options are: NO STATIC DYNAMIC YES AUTO.")

    if(NOT ${${env_name}} MATCHES "^(NO|YES|AUTO|STATIC|DYNAMIC)$")
      message(FATAL_ERROR "Invalid value ${env_name} for ${env_name}")
    endif()

    set(cargo_cmd 
      cargo rustc
      --target-dir=${cargo_target_dir}
      --package=${cargo_name}
    )
    set(rustc_extra_args)

    # Configure debug/release options
    if(CMAKE_BUILD_TYPE MATCHES "Debug")
      set(cargo_cmd ${cargo_cmd} --profile=dev)
      # set(output_path "${output_path}/debug")
    elseif(CMAKE_BUILD_TYPE MATCHES "Release")
      set(cargo_cmd ${cargo_cmd} --profile=release)
      # set(output_path "${output_path}/release")
    elseif(CMAKE_BUILD_TYPE MATCHES "RelWithDebInfo")
      set(cargo_cmd ${cargo_cmd} --profile=release)
      # set(output_path "${output_path}/release")
    elseif(CMAKE_BUILD_TYPE MATCHES "MinSizeRel")
      set(cargo_cmd ${cargo_cmd} --profile=release)
      set(rustc_extra_args ${rustc_extra_args} -C strip=debuginfo)
    endif()

    # Used by build.rs
    set(env_args -E env CMAKE_SOURCE_DIR=${CMAKE_SOURCE_DIR}
        CMAKE_BINARY_DIR=${CMAKE_BINARY_DIR})

    # Commands for dynamic and static; these don't get invoked until our
    # target calls them
    add_custom_command(
      OUTPUT ${staticlib_name}
      # We set make_static_lib to generate the correct symbols
      # equivalent of `COMPILE_DEFINITIONS "MYSQL_DYNAMIC_PLUGIN$...` for C plugins
      # Todos:
      # TARGET_LINK_LIBRARIES (${target} mysqlservices ${ARG_LINK_LIBRARIES})
      COMMAND ${CMAKE_COMMAND} ${env_args}
        ${cargo_cmd} --crate-type=staticlib
        -- ${rustc_extra_args} --cfg=make_static_lib
      WORKING_DIRECTORY ${rust_dir}
      COMMENT "start cargo for ${target_name} with '${cargo_cmd}' static"
      VERBATIM
    )
    add_custom_command(
      OUTPUT ${dynlib_name}
      COMMAND ${CMAKE_COMMAND} ${env_args}
        ${cargo_cmd} --crate-type=cdylib
        -- ${rustc_extra_args}
      WORKING_DIRECTORY ${rust_dir}
      COMMENT "start cargo for ${target_name} with '${cargo_cmd}' dynamic"
      VERBATIM
    )

    if(
      ${${env_name}} MATCHES "(STATIC|AUTO|YES)" AND NOT ARG_MODULE_ONLY
      AND NOT ARG_CLIENT
    )
      # Build a staticlib
      message("building rust static ${target_name}")

      add_custom_target(${target_name} ALL
        COMMAND echo "invoking cargo for ${target_name}"
        DEPENDS ${staticlib_name}
      )

      # Update mysqld dependencies
      SET (MYSQLD_STATIC_PLUGIN_LIBS ${MYSQLD_STATIC_PLUGIN_LIBS} 
        ${target_name} ${ARG_LINK_LIBRARIES} CACHE INTERNAL "" FORCE)

      message("more to do here...")

    elseif(
      ${${env_name}} MATCHES "(DYNAMIC|AUTO|YES)"
      AND NOT ARG_STATIC_ONLY AND NOT WITHOUT_DYNAMIC_PLUGINS
    )
      # Build a dynamiclib
      message("building rust dynamic ${target_name}")

      add_custom_target(${target_name} ALL
        COMMAND echo "invoking cargo for ${target_name}"
        DEPENDS ${dynlib_name}
      )
    
      # IF(TARGET ${target})
      #   GET_TARGET_PROPERTY(plugin_type ${target} TYPE)
      #   STRING(REPLACE "_LIBRARY" "" plugin_type ${plugin_type})
      #   SET(have_target 1)
      # ELSE()
      #   SET(plugin_type)
      #   SET(have_target 0)
      # ENDIF()
      # IF(ARG_STORAGE_ENGINE)
      #   ADD_FEATURE_INFO(${plugin} ${have_target} "Storage Engine ${plugin_type}")
      # ELSEIF(ARG_CLIENT)
      #   ADD_FEATURE_INFO(${plugin} ${have_target} "Client plugin ${plugin_type}")
      # ELSE()
      #   ADD_FEATURE_INFO(${plugin} ${have_target} "Server plugin ${plugin_type}")
      # ENDIF()
      # ENDIF(NOT WITHOUT_SERVER OR ARG_CLIENT)

    endif()

      # add_dependencies()(${target} GenError ${ARG_DEPENDS})
      # target_include_directories(${target_name} )
      # add_library(${target_name} MODULE ${dynlib_name} IMPORTED)
      # target_link_libraries(your_cpp_bin PUBLIC rust-lib)
      # add_dependencies(${target_name})
      


      # add_library(${target_name} MODULE IMPORTED GLOBAL)
      # # add_library(${target_name} STATIC IMPORTED GLOBAL)
      # add_dependencies(${target_name} ${target_name}_target)

      # # specify where the library is and where to find the headers
      # set_target_properties(${target_name}
      #     PROPERTIES
      #     IMPORTED_LOCATION ${LIB_FILE}
      #     INTERFACE_INCLUDE_DIRECTORIES ${LIB_HEADER_FOLDER})
      # set_target_properties(${target_name} PROPERTIES
      #   CXX_STANDARD 11
      #   CXX_STANDARD_REQUIRED ON
      #   LINKER_LANGUAGE CXX
      # )


    # target_include_directories(quiche INTERFACE ${quiche_source_SOURCE_DIR}/include)

    # add_custom_command(
    #         OUTPUT libquiche.a
    #         COMMAND cargo rustc 
    #         WORKING_DIRECTORY ${quiche_source_SOURCE_DIR}
    #         COMMENT "start building quiche ${QUICHE_BUILD_ARGS}"
    #         VERBATIM
    # )
  endforeach()

  # get_cmake_property()(ALL_VARS VARIABLES)
  # foreach (V ${ALL_VARS})
  # if(V MATCHES "^PLUGIN_" AND ${V} MATCHES "YES")
  # string(SUBSTRING ${V} 7 -1 plugin)
  # string(TOLOWER ${plugin} target)
  # IF (NOT TARGET ${target})
  # MESSAGE(FATAL_ERROR "Plugin ${plugin} cannot be built")
  # endif()
  # endif()
  # endforeach()
endmacro()

macro (create_dylib_install)
      # No clue wtf this does
      # if(ARG_COMPONENT)
      #   if(CPACK_COMPONENTS_ALL AND
      #     NOT CPACK_COMPONENTS_ALL MATCHES ${ARG_COMPONENT}
      #     AND INSTALL_SYSCONF2DIR
      #   )
      #     if(ARG_STORAGE_ENGINE)
      #       string(REPLACE "-" "_" ver ${SERVER_VERSION})
      #       set(ver " = ${ver}-%{release}")
      #     else()
      #       set(ver "")
      #     endif()
      #     string(TOUPPER ${ARG_COMPONENT} ARG_COMPONENT_UPPER)
      #     set(CPACK_COMPONENT_${ARG_COMPONENT_UPPER}SYMLINKS_GROUP ${ARG_COMPONENT} PARENT_SCOPE)
      #     set(CPACK_COMPONENT_${ARG_COMPONENT_UPPER}_GROUP ${ARG_COMPONENT} PARENT_SCOPE)
      #     set(CPACK_COMPONENTS_ALL ${CPACK_COMPONENTS_ALL} ${ARG_COMPONENT} ${ARG_COMPONENT}Symlinks)
      #     set(CPACK_COMPONENTS_ALL ${CPACK_COMPONENTS_ALL} PARENT_SCOPE)

      #     if(NOT ARG_CLIENT)
      #       set(CPACK_RPM_${ARG_COMPONENT}_PACKAGE_REQUIRES "MariaDB-server${ver}" PARENT_SCOPE)
      #     endif()
      #     set(CPACK_RPM_${ARG_COMPONENT}_USER_FILELIST ${ignored} PARENT_SCOPE)
      #     if(ARG_VERSION)
      #       set(CPACK_RPM_${ARG_COMPONENT}_PACKAGE_VERSION ${SERVER_VERSION}_${ARG_VERSION} PARENT_SCOPE)
      #       SET_PLUGIN_DEB_VERSION(${target} ${SERVER_VERSION}-${ARG_VERSION})
      #     endif()
      #     if(NOT ARG_CLIENT AND UNIX)
      #       if(NOT ARG_CONFIG)
      #         set(ARG_CONFIG "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${target}.cnf")
      #         FILE(WRITE ${ARG_CONFIG} "[mariadb]\nplugin-load-add=${ARG_MODULE_OUTPUT_NAME}.so\n")
      #       endif()
      #       set(CPACK_RPM_${ARG_COMPONENT}_USER_FILELIST ${ignored} "%config(noreplace) ${INSTALL_SYSCONF2DIR}/*" PARENT_SCOPE)
      #       set(CPACK_RPM_${ARG_COMPONENT}_POST_INSTALL_SCRIPT_FILE ${CMAKE_SOURCE_DIR}/support-files/rpm/plugin-postin.sh PARENT_SCOPE)
      #       set(CPACK_RPM_${ARG_COMPONENT}_POST_TRANS_SCRIPT_FILE ${CMAKE_SOURCE_DIR}/support-files/rpm/server-posttrans.sh PARENT_SCOPE)
      #     endif()
      #   endif()
      # else()
      #   set(ARG_COMPONENT Server)
      # endif()
  endmacro()
      # MYSQL_INSTALL_TARGETS(${target} DESTINATION ${INSTALL_PLUGINDIR} COMPONENT ${ARG_COMPONENT})
      # if(ARG_CONFIG AND INSTALL_SYSCONF2DIR)
      #   install(FILES ${ARG_CONFIG} COMPONENT ${ARG_COMPONENT} DESTINATION ${INSTALL_SYSCONF2DIR})
      # endif()
      # GET_FILENAME_COMPONENT(subpath ${CMAKE_CURRENT_SOURCE_DIR} NAME)
      # IF(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/mysql-test")
      #   INSTALL_MYSQL_TEST("${CMAKE_CURRENT_SOURCE_DIR}/mysql-test/" "plugin/${subpath}")
      # ENDIF()
