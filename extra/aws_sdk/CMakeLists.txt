OPTION(AWS_SDK_EXTERNAL_PROJECT  "Allow to download and build AWS C++ SDK" OFF)
INCLUDE(aws_sdk)
INCLUDE(ExternalProject)

GET_PROPERTY(SDK_LIBS GLOBAL PROPERTY AWS_SDK_LIBS)
LIST(LENGTH SDK_LIBS SDK_LIBS_COUNT)
IF(SDK_LIBS_COUNT EQUAL 0)
  RETURN()
ENDIF()

CHECK_AWS_SDK("${SDK_LIBS}" RETVAL REASON)
IF(NOT RETVAL)
  MESSAGE(FATAL_ERROR 
   "AWS C++ will not be built (${REASON}), but dependency on following components is found ${SDK_LIBS}.
   Use CHECK_AWS_SDK() function before trying to build with SDK components")
ENDIF()
 
IF(NOT AWS_SDK_EXTERNAL_PROJECT)
  # System aws sdk libraries found
  RETURN()
ENDIF()

SET(all_libs
  aws-crt-cpp
  aws-c-event-stream
  aws-c-common
  aws-c-http
  aws-c-mqtt
  aws-c-cal
  aws-c-auth
  aws-c-io
  aws-checksums
  aws-c-s3
  aws-c-sdkutils
  aws-c-compression
  aws-cpp-sdk-core
)
IF(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  LIST(APPEND all_libs s2n)
ENDIF()

FOREACH(lib ${SDK_LIBS})
  LIST(APPEND all_libs aws-cpp-sdk-${lib})
ENDFOREACH()
LIST(REMOVE_DUPLICATES all_libs)

SET(byproducts)
MAKE_DIRECTORY(${PROJECT_BINARY_DIR}/extra/aws_sdk/aws_sdk_cpp/include)
FOREACH(lib ${all_libs})
  ADD_LIBRARY(${lib} UNKNOWN IMPORTED GLOBAL)
  ADD_DEPENDENCIES(${lib} aws_sdk_cpp)
  
  SET (loc "${CMAKE_CURRENT_BINARY_DIR}/aws_sdk_cpp/lib/${CMAKE_STATIC_LIBRARY_PREFIX}${lib}${CMAKE_STATIC_LIBRARY_SUFFIX}")
  IF(CMAKE_VERSION VERSION_GREATER "3.1")
    SET(byproducts ${byproducts} BUILD_BYPRODUCTS ${loc})
  ENDIF()
  SET_TARGET_PROPERTIES(${lib} PROPERTIES IMPORTED_LOCATION ${loc})
  TARGET_INCLUDE_DIRECTORIES(${lib} INTERFACE ${PROJECT_BINARY_DIR}/extra/aws_sdk/aws_sdk_cpp/include)
ENDFOREACH()

# Define link dependencies  between AWS C libs
TARGET_LINK_LIBRARIES(aws-cpp-sdk-core INTERFACE aws-crt-cpp aws-c-event-stream aws-c-common)
TARGET_LINK_LIBRARIES(aws-crt-cpp INTERFACE aws-c-http aws-c-mqtt aws-c-cal aws-c-auth aws-c-common aws-c-io
  aws-checksums aws-c-event-stream aws-c-s3 aws-c-sdkutils)
TARGET_LINK_LIBRARIES(aws-c-event-stream INTERFACE  aws-c-io aws-c-common aws-checksums)
TARGET_LINK_LIBRARIES(aws-c-http INTERFACE aws-c-io aws-c-compression aws-c-cal aws-c-common)
TARGET_LINK_LIBRARIES(aws-c-mqtt INTERFACE aws-c-http aws-c-io aws-c-common aws-c-cal aws-c-common)
TARGET_LINK_LIBRARIES(aws-c-auth INTERFACE aws-c-sdkutils  aws-c-cal  aws-c-http  aws-c-io  aws-c-common)
TARGET_LINK_LIBRARIES(aws-c-io INTERFACE aws-c-common  aws-c-cal)
TARGET_LINK_LIBRARIES(aws-checksums INTERFACE aws-c-common)
TARGET_LINK_LIBRARIES(aws-c-compression INTERFACE aws-c-common)
TARGET_LINK_LIBRARIES(aws-c-s3 INTERFACE aws-c-auth  aws-checksums aws-c-http aws-c-io aws-c-cal aws-c-common aws-c-sdkutils  aws-c-common)
TARGET_LINK_LIBRARIES(aws-c-sdkutils INTERFACE aws-c-common)

IF(WIN32)
  TARGET_LINK_LIBRARIES(aws-c-io INTERFACE bcrypt crypt32 secur32 ncrypt ws2_32)
  TARGET_LINK_LIBRARIES(aws-c-common INTERFACE  shlwapi)
  TARGET_LINK_LIBRARIES(aws-cpp-sdk-core INTERFACE winhttp wininet version userenv)
ELSE()
  TARGET_LINK_LIBRARIES(aws-c-cal INTERFACE OpenSSL::Crypto)
  TARGET_LINK_LIBRARIES(aws-cpp-sdk-core INTERFACE ZLIB::ZLIB CURL::libcurl)

  # Dependencies below are from CMakeLists.txt for aws-c-common
  SET(THREADS_PREFER_PTHREAD_FLAG ON)
  FIND_PACKAGE(Threads REQUIRED)
  TARGET_LINK_LIBRARIES(aws-c-common INTERFACE ${CMAKE_DL_LIBS} Threads::Threads)
  IF(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    TARGET_LINK_LIBRARIES(aws-c-common INTERFACE s2n m rt)
  ELSEIF(APPLE)
    TARGET_LINK_LIBRARIES(aws-c-common INTERFACE "-framework CoreFoundation")
    TARGET_LINK_LIBRARIES(aws-c-io INTERFACE "-framework Security" "-framework Network")
  ENDIF()
ENDIF()

FOREACH(lib ${SDK_LIBS})
  TARGET_LINK_LIBRARIES(aws-cpp-sdk-${lib} INTERFACE aws-cpp-sdk-core)
ENDFOREACH()

SET(GIT_TAG "1.11.582")

LIST(REMOVE_DUPLICATES SDK_LIBS)
STRING( REPLACE ";" "!" SDK_LIBS_STR "${SDK_LIBS}")

IF(MSVC)
 SET(BUILD_AND_INSTALL_COMMANDS
   BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config RelWithDebInfo --verbose --parallel
   INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --config RelWithDebInfo)
 SET(BUILD_TYPE -DCMAKE_BUILD_TYPE=RelWithDebInfo)
ELSE()
 SET(BUILD_AND_INSTALL_COMMANDS)
 SET(BUILD_TYPE)
ENDIF()

ExternalProject_Add(
  aws_sdk_cpp
  GIT_REPOSITORY "https://github.com/awslabs/aws-sdk-cpp.git"
  GIT_TAG ${GIT_TAG}
  GIT_SHALLOW TRUE
  UPDATE_COMMAND ""
  SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/aws-sdk-cpp"
  LIST_SEPARATOR !
  ${byproducts}
  CMAKE_ARGS
    ${BUILD_TYPE}
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DBUILD_ONLY=${SDK_LIBS_STR}
    -DBUILD_SHARED_LIBS=OFF
    -DFORCE_SHARED_CRT=ON
    -DENABLE_TESTING=OFF
    -DAWS_WARNINGS_ARE_ERRORS=OFF
    ${EXTRA_SDK_CMAKE_FLAGS}
    -DCMAKE_INSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/aws_sdk_cpp
    -DCMAKE_INSTALL_LIBDIR=lib
  TEST_COMMAND ""
  ${BUILD_AND_INSTALL_COMMANDS}
)

SET_TARGET_PROPERTIES(aws_sdk_cpp PROPERTIES EXCLUDE_FROM_ALL TRUE)
