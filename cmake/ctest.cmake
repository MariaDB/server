
INCLUDE(CMakeParseArguments)

MACRO(MY_ADD_TEST name)
  ADD_TEST(${name} ${name}-t)
ENDMACRO()

MACRO(MY_ADD_TESTS)
  CMAKE_PARSE_ARGUMENTS(ARG "" "EXT" "LINK_LIBRARIES" ${ARGN})

  INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/include
                      ${CMAKE_SOURCE_DIR}/unittest/mytap)

  IF (NOT ARG_EXT)
    SET(ARG_EXT "c")
  ENDIF()

  FOREACH(name ${ARG_UNPARSED_ARGUMENTS})
    ADD_EXECUTABLE(${name}-t "${name}-t.${ARG_EXT}")
    TARGET_LINK_LIBRARIES(${name}-t mytap ${ARG_LINK_LIBRARIES})
    MY_ADD_TEST(${name})
  ENDFOREACH()
ENDMACRO()

