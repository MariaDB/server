MACRO (MYSQL_CHECK_NUMA)

  STRING(TOLOWER "${WITH_NUMA}" WITH_NUMA_LOWERCASE)

  IF(NOT WITH_NUMA)
    MESSAGE_ONCE(numa "WITH_NUMA=OFF: NUMA memory allocation policy disabled")

  ELSEIF(NOT WITH_NUMA_LOWERCASE STREQUAL "auto" AND NOT WITH_NUMA_LOWERCASE STREQUAL "on")
      MESSAGE(FATAL_ERROR "Wrong value for WITH_NUMA")

  ELSEIF(CMAKE_SYSTEM_NAME MATCHES "Linux")
    CHECK_INCLUDE_FILES(numa.h HAVE_NUMA_H)
    CHECK_INCLUDE_FILES(numaif.h HAVE_NUMAIF_H)

    IF(HAVE_NUMA_H AND HAVE_NUMAIF_H)
      SET(SAVE_CMAKE_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES})
      SET(CMAKE_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES} numa)
      CHECK_C_SOURCE_COMPILES(
      "
      #include <numa.h>
      #include <numaif.h>
      int main()
      {
         struct bitmask *all_nodes= numa_all_nodes_ptr;
         set_mempolicy(MPOL_DEFAULT, 0, 0);
         return all_nodes != NULL;
      }"
      HAVE_LIBNUMA)
      SET(CMAKE_REQUIRED_LIBRARIES ${SAVE_CMAKE_REQUIRED_LIBRARIES})
      IF(HAVE_LIBNUMA)
        ADD_DEFINITIONS(-DHAVE_LIBNUMA=1)
        SET(NUMA_LIBRARY "numa")
      ENDIF()
    ENDIF()

    ADD_FEATURE_INFO(NUMA HAVE_LIBNUMA "NUMA memory allocation policy")
    IF(WITH_NUMA_LOWERCASE STREQUAL "auto" AND HAVE_LIBNUMA)
      MESSAGE_ONCE(numa "WITH_NUMA=AUTO: NUMA memory allocation policy enabled")
    ELSEIF(WITH_NUMA_LOWERCASE STREQUAL "auto" AND NOT HAVE_LIBNUMA)
      MESSAGE_ONCE(numa "WITH_NUMA=AUTO: NUMA memory allocation policy disabled")
    ELSEIF(HAVE_LIBNUMA)
      MESSAGE_ONCE(numa "WITH_NUMA=ON: NUMA memory allocation policy enabled")
    ELSE()
      # Forget it in cache, abort the build.
      UNSET(WITH_NUMA CACHE)
      UNSET(NUMA_LIBRARY CACHE)
      MESSAGE(FATAL_ERROR "WITH_NUMA=ON: Could not find NUMA headers/libraries")
    ENDIF()

 ENDIF()

ENDMACRO()

