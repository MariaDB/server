# update submodules automatically

OPTION(UPDATE_SUBMODULES "Update submodules automatically" ON)
IF(NOT UPDATE_SUBMODULES)
  RETURN()
ENDIF()

IF(GIT_EXECUTABLE AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
  EXECUTE_PROCESS(COMMAND "${GIT_EXECUTABLE}" config --get cmake.update-submodules
                  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                  OUTPUT_VARIABLE cmake_update_submodules
                  RESULT_VARIABLE git_config_get_result)
  IF(git_config_get_result EQUAL 128 OR cmake_update_submodules MATCHES no)
    SET(update_result 0)
  ELSEIF (cmake_update_submodules MATCHES force)
    MESSAGE(STATUS "Updating submodules (forced)")
    EXECUTE_PROCESS(COMMAND "${GIT_EXECUTABLE}" submodule update --init --force
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                    RESULT_VARIABLE update_result)
  ELSEIF (cmake_update_submodules MATCHES yes)
    EXECUTE_PROCESS(COMMAND "${GIT_EXECUTABLE}" submodule update --init
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                    RESULT_VARIABLE update_result)
  ELSE()
    MESSAGE(STATUS "Updating submodules")
    EXECUTE_PROCESS(COMMAND "${GIT_EXECUTABLE}" submodule update --init
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                    RESULT_VARIABLE update_result)
  ENDIF()
ENDIF()

IF(update_result OR NOT EXISTS ${CMAKE_SOURCE_DIR}/libmariadb/CMakeLists.txt)
  MESSAGE(FATAL_ERROR "No MariaDB Connector/C! Run
    git submodule update --init
Then restart the build.
")
ENDIF()
