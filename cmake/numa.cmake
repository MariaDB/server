MACRO (MYSQL_CHECK_NUMA)

  IF(CMAKE_SYSTEM_NAME MATCHES "Linux")
    CHECK_INCLUDE_FILES(numa.h HAVE_NUMA_H)
    CHECK_INCLUDE_FILES(numaif.h HAVE_NUMAIF_H)

    IF(HAVE_NUMA_H AND HAVE_NUMAIF_H)
      OPTION(WITH_NUMA "Explicitly set NUMA memory allocation policy" ON)
    ELSE()
      OPTION(WITH_NUMA "Explicitly set NUMA memory allocation policy" OFF)
    ENDIF()

    IF(WITH_NUMA AND HAVE_NUMA_H AND HAVE_NUMAIF_H)
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
    IF(WITH_NUMA AND NOT HAVE_LIBNUMA)
      # Forget it in cache, abort the build.
      UNSET(WITH_NUMA CACHE)
      UNSET(NUMA_LIBRARY CACHE)
      MESSAGE(FATAL_ERROR "Could not find numa headers/libraries")
    ENDIF()
 ENDIF()

ENDMACRO()

