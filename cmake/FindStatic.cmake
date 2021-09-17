MACRO(FIND_STATIC VAR NAME)
  FIND_LIBRARY(
    sLIBRARY 
    NAMES lib${NAME}.a
    )
  IF(sLIBRARY)
    MESSAGE(STATUS "* Found static variant for ${NAME}: ${sLIBRARY}")
    SET(${VAR} ${sLIBRARY})
  ELSE()
    MESSAGE(STATUS "Static variant for ${NAME} is not found...")
  ENDIF()
ENDMACRO()
