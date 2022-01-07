IF(NOT RPM AND NOT DEB)
  #
  # use -DEXTRA_FILES='/path/to/file=where/to/install;/bin/dd=bin;...'
  #
  FOREACH(f ${EXTRA_FILES})
    STRING(REGEX REPLACE "=.*$" "" from ${f})
    STRING(REGEX REPLACE "^.*=" "" to   ${f})
    INSTALL(PROGRAMS ${from} DESTINATION ${to})
  ENDFOREACH()
ENDIF()
