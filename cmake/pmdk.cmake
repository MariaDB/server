# pmdk version is 1.5
# -DBUILD_WITH_PMDK = 1 is default

MACRO (FIND_SYSTEM_PMDK)
    FIND_PATH(PATH_TO_PMDK NAMES libpmem.h)
    IF (PATH_TO_PMDK)
        SET(SYSTEM_PMDK_FOUND 1)
        SET(PMDK_INCLUDE_DIR ${PATH_TO_PMDK})
    ENDIF()
ENDMACRO()

MACRO (FIND_SYSTEM_NUMA)
    FIND_PATH(PATH_TO_NUMA NAMES numa.h)
    IF (PATH_TO_NUMA)
        SET(SYSTEM_NUMA_FOUND 1)
        SET(NUMA_INCLUDE_DIR ${PATH_TO_NUMA})
    ENDIF()
ENDMACRO()

MACRO (MYSQL_CHECK_PMDK)
    IF (NOT BUILD_WITH_PMDK STREQUAL "system")
        SET(BUILD_WITH_PMDK "bundled")
    ENDIF()

    IF (BUILD_WITH_PMDK STREQUAL "bundled")
        SET(PMDK_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/extra/pmdk/include)
        SET(NUMA_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/extra/pmdk/include)
    ELSEIF(BUILD_WITH_PMDK STREQUAL "system")
        FIND_SYSTEM_PMDK()
        IF (NOT SYSTEM_PMDK_FOUND)
            MESSAGE(FATAL_ERROR "Cannot find system pmdk head file. You need to "
                    "add the required head file.\n"
                    "You can also use the bundled version by specifyng "
                    "-DBUILD_WITH_PMDK=bundled.")
        ENDIF()
        FIND_SYSTEM_NUMA()
        IF (NOT SYSTEM_NUMA_FOUND)
            MESSAGE(FATAL_ERROR "Cannot find system numa head file. You need to "
                    "add the required head file.\n"
                    "You can also use the bundled version by specifyng "
                    "-DBUILD_WITH_PMDK=bundled.")
        ENDIF()
    ELSE()
        MESSAGE(FATAL_ERROR "BUILD_WITH_PMDK must be bundled or system")
    ENDIF()

    MESSAGE(STATUS "PMDK_INCLUDE_DIR ${PMDK_INCLUDE_DIR}")
    INCLUDE_DIRECTORIES(SYSTEM ${PMDK_INCLUDE_DIR})
    MESSAGE(STATUS "NUMA_INCLUDE_DIR ${NUMA_INCLUDE_DIR}")
    INCLUDE_DIRECTORIES(SYSTEM ${NUMA_INCLUDE_DIR})

ENDMACRO()
