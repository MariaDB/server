OPTION(UPDATE_SUBMODULES "Update submodules automatically" ON)

IF(NOT UPDATE_SUBMODULES)
  SET(SUBMODULE_UPDATE_CONFIG_MESSAGE "Disabled by -DUPDATE_SUBMODULES=OFF")
ELSEIF(NOT GIT_EXECUTABLE)
  SET(SUBMODULE_UPDATE_CONFIG_MESSAGE "git executable was not found")
ELSEIF(NOT EXISTS "${CMAKE_SOURCE_DIR}/.git")
  SET(SUBMODULE_UPDATE_CONFIG_MESSAGE "Not inside a git repository")
ELSE()
  EXECUTE_PROCESS(COMMAND "${GIT_EXECUTABLE}" config --get cmake.update-submodules
                  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                  OUTPUT_VARIABLE CMAKE_UPDATE_SUBMODULES
                  RESULT_VARIABLE git_config_get_result)
  IF(CMAKE_UPDATE_SUBMODULES MATCHES no)
    SET(update_result 0)
    SET(SUBMODULE_UPDATE_CONFIG_MESSAGE "Disabled by git config. To enable set cmake.update-submodules to 'yes', or 'force': ${GIT_EXECUTABLE} config cmake.update-submodules yes")
  ELSEIF(git_config_get_result EQUAL 128)
    SET(SUBMODULE_UPDATE_CONFIG_MESSAGE "Git executable ${GIT_EXECUTABLE} failed to run")
  ENDIF()
ENDIF()

FUNCTION(ADD_SUBMODULE dir)
  IF (ARGV1)
    SET(file "${ARGV1}")
  ELSE()
    SET(file CMakeLists.txt)
  ENDIF()
  IF(SUBMODULE_UPDATE_CONFIG_MESSAGE)
    IF(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${dir}/${file})
      MESSAGE(FATAL_ERROR "Cannot download ${CMAKE_CURRENT_SOURCE_DIR}/${dir} submodule: ${SUBMODULE_UPDATE_CONFIG_MESSAGE}")
    ENDIF()
  ELSE()
    MESSAGE(STATUS "Downloading ${CMAKE_CURRENT_SOURCE_DIR}/${dir} submodule...")
    SET(UPDATE_SUBMODULES_COMMAND
        "${GIT_EXECUTABLE}" submodule update --init --recursive --depth 1)
    IF(CMAKE_UPDATE_SUBMODULES MATCHES force)
      EXECUTE_PROCESS(COMMAND ${UPDATE_SUBMODULES_COMMAND} --force ${dir}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                      RESULT_VARIABLE update_result)
    ELSE()
      EXECUTE_PROCESS(COMMAND ${UPDATE_SUBMODULES_COMMAND} ${dir}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                      RESULT_VARIABLE update_result)
    ENDIF()
    IF(update_result OR NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${dir}/${file})
      MESSAGE(FATAL_ERROR "Failed to download ${CMAKE_CURRENT_SOURCE_DIR}/${dir} submodule")
    ENDIF()
  ENDIF()
ENDFUNCTION()

MACRO(ADD_SUBMODULE_SUBDIRECTORY dir)
  ADD_SUBMODULE(${dir})
  ADD_SUBDIRECTORY(${dir})
ENDMACRO()
