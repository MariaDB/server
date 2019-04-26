set_property(DIRECTORY APPEND PROPERTY COMPILE_DEFINITIONS _GNU_SOURCE DONT_DEPRECATE_ERRNO)

set(tools tokudb_dump tokuftdump tokuft_logprint tdb-recover ftverify)
foreach(tool ${tools})
  add_executable(${tool} ${tool}.cc)
  add_dependencies(${tool} install_tdb_h)
  target_link_libraries(${tool} ${LIBTOKUDB}_static ft_static z lzma snappy ${LIBTOKUPORTABILITY}_static ${CMAKE_THREAD_LIBS_INIT} ${EXTRA_SYSTEM_LIBS})

  # detect when we are being built as a subproject
  if (DEFINED MYSQL_PROJECT_NAME_DOCSTRING)
    # if we are compiling under a MySQL variant, this macro must be present
    GET_MYSQL_VERSION()


    if ((CMAKE_BUILD_TYPE MATCHES "Debug") AND
        (CMAKE_CXX_FLAGS_DEBUG MATCHES " -DENABLED_DEBUG_SYNC"))
      if (MYSQL_BASE_VERSION VERSION_EQUAL "8.0")
        target_link_libraries(${tool} sql_main sql_gis sql_main sql_dd sql_gis binlog rpl master slave ${ICU_LIBRARIES})
      else ()
        target_link_libraries(${tool} sql binlog rpl master slave)
      endif ()
    else ()
      target_link_libraries(${tool} perconaserverclient)
    endif ()
  endif ()

  add_space_separated_property(TARGET ${tool} COMPILE_FLAGS -fvisibility=hidden)
endforeach(tool)

# link in math.h library just for this tool.
target_link_libraries(ftverify m)

install(TARGETS tokuftdump DESTINATION bin COMPONENT tokukv_tools)
install(TARGETS tokuft_logprint DESTINATION bin COMPONENT tokukv_tools)
