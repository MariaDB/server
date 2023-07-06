

# TODO: just put this in cargo.toml and parse it
# CMAKE_PARSE_ARGUMENTS(ARG
# "STORAGE_ENGINE;STATIC_ONLY;MODULE_ONLY;MANDATORY;DEFAULT;DISABLED;NOT_EMBEDDED;RECOMPILE_FOR_EMBEDDED;CLIENT"
# "MODULE_OUTPUT_NAME;STATIC_OUTPUT_NAME;COMPONENT;CONFIG;VERSION"
# "LINK_LIBRARIES;DEPENDS"
# ${ARGN}
# )

macro(MYSQL_ADD_RUST_PLUGIN)
  if(not WITHOUT_SERVER or ARG_CLIENT)
    INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/include
      ${CMAKE_SOURCE_DIR}/sql
      ${PCRE_INCLUDES}
      ${SSL_INCLUDE_DIRS}
      ${ZLIB_INCLUDE_DIR})

    # NO - not at all
    # YES - static if possible, otherwise dynamic if possible, otherwise abort
    # AUTO - static if possible, otherwise dynamic, if possible
    # STATIC - static if possible, otherwise not at all
    # DYNAMIC - dynamic if possible, otherwise not at all
    SET(PLUGIN_${plugin} ${howtobuild}
      CACHE STRING "How to build plugin ${plugin}. Options are: NO STATIC DYNAMIC YES AUTO.")
  endif()
endmacro()

macro(CONFIGURE_RUST_PLUGINS)
  include(FetchContent)

  FetchContent_Declare(
    Corrosion
    GIT_REPOSITORY https://github.com/corrosion-rs/corrosion.git
    GIT_TAG v0.4.1
  )
  FetchContent_MakeAvailable(Corrosion)

  set(rust_dir "${CMAKE_SOURCE_DIR}/rust")
  set(CARGO_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/rust_target")
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

    message("IDENT ${target_name}")

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
    set(${env_name} howtobuild CACHE STRING
      "How to build plugin ${cargo_name}. Options are: NO STATIC DYNAMIC YES AUTO.")

    message("varname ${env_name} varval ${${env_name}} htb ${howtobuild}")

    if(NOT ${${env_name}} MATCHES "^(NO|YES|AUTO|STATIC|DYNAMIC)$")
      message(FATAL_ERROR "Invalid value ${env_name} for ${env_name}")
    endif()

    set(cargo_cmd cargo rustc --package=${cargo_name})
    set(rustc_extra_args "")
    # set(output_path "${CARGO_TARGET_DIR}")
    
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
      set(rustc_extra_args "${rustc_extra_args} -C strip=debuginfo")
    endif()

    if(${${env_name}} MATCHES "(STATIC|AUTO|YES)" AND NOT ARG_MODULE_ONLY
        AND NOT ARG_CLIENT)
      message("rust static")
      set(should_build "true")
      set(cargo_cmd ${cargo_cmd} --crate-type=staticlib)
      # add_library(${target_name} STATIC ${staticlib_name})
      # set_target_properties(${target_name}
      #   PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
      # target_link_libraries(
      #   ${target_name}
      #   debug "${CARGO_TARGET_DIR}/debug/${staticlib_name}"
      #   optimized "${CARGO_TARGET_DIR}/release/${staticlib_name}"
      # )
      message("more to do here...")
    elseif(${${env_name}} MATCHES "(DYNAMIC|AUTO|YES)"
        AND NOT ARG_STATIC_ONLY AND NOT WITHOUT_DYNAMIC_PLUGINS)
      message("rust dynamic")
      set(should_build "true")
      # set(output_path "${output_path}/${dynlib_name}")
      set(cargo_cmd ${cargo_cmd} --crate-type=cdylib)
      # add_library(${target_name} SHARED ${dynlib_name})
      # set_target_properties(${target_name}
      #   PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
      # target_link_libraries(
      #   ${target_name}
      #   debug "${CARGO_TARGET_DIR}/debug/${dynlib_name}"
      #   optimized "${CARGO_TARGET_DIR}/release/${dynlib_name}"
      # )
    endif()

    if(should_build)
      message("building rust for ${target_name} with cargo name ${cargo_name}")
      # add_dependencies()(${target} GenError ${ARG_DEPENDS})
      # target_include_directories(${target_name} )

      # add_custom_target
      # corrosion_import_crate(
      #   MANIFEST_PATH "${CMAKE_SOURCE_DIR}/rust/Cargo.toml"
      #   CRATES ${cargo_name}
      # )

      # add_library(${target_name} MODULE ${dynlib_name} IMPORTED)
      # target_link_libraries(your_cpp_bin PUBLIC rust-lib)
      # add_custom_target(
      #   ${target_name}
      #   # OUTPUT ${target_name}
      #   # OUTPUT ${dynlib_name}
      #   COMMAND ${cargo_cmd} -- ${rustc_extra_args}
      #   WORKING_DIRECTORY ${rust_dir}
      #   COMMENT "start building ${target_name} '${cargo_cmd}'"
      #   VERBATIM
      # )
      # add_dependencies(${target_name})
      
      add_custom_command(
        OUTPUT ${dynlib_name}
        # OUTPUT ${dynlib_name}
        # COMMAND which cargo | xargs echo
        COMMAND ${cargo_cmd}
        #  -- ${rustc_extra_args}
        WORKING_DIRECTORY ${rust_dir}
        COMMENT "start building ${target_name} '${cargo_cmd}'"
        VERBATIM
      )
      add_custom_target(${target_name}_target
        ALL
        command echo "starting thing"
        COMMENT "MAKING TARGET"
        DEPENDS ${dynlib_name}
      )
      # add_library(${target_name} MODULE IMPORTED GLOBAL)
      # # add_library(${target_name} STATIC IMPORTED GLOBAL)
      # add_dependencies(${target_name} ${target_name}_target)

      # # specify where the library is and where to find the headers
      # set_target_properties(${target_name}
      #     PROPERTIES
      #     IMPORTED_LOCATION ${LIB_FILE}
      #     INTERFACE_INCLUDE_DIRECTORIES ${LIB_HEADER_FOLDER})

      

      # message("IDENT: ${target_name}")
      # set_target_properties(${target_name} PROPERTIES
      #   CXX_STANDARD 11
      #   CXX_STANDARD_REQUIRED ON
      #   LINKER_LANGUAGE CXX
      # )


    endif()

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
