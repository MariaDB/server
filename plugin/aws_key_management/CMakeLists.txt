# We build parts of AWS C++ SDK as CMake external project
# The restrictions of the SDK (https://github.com/awslabs/aws-sdk-cpp/blob/master/README.md)
# are
 
# - OS : Windows,Linux or OSX
# - C++11 compiler : VS2013+, gcc 4.8+, clang 3.3+
# - libcurl development package needs to be present on Unixes
#
# If we build SDK outselves, we'll need require GIT to be present on the build machine


# Give message why the building this plugin is skipped (only if -DVERBOSE is defined)
# or if plugin is explicitly requested to build. Then bail out.
MACRO(SKIP_AWS_PLUGIN msg)
  MESSAGE_ONCE(SKIP_AWS_PLUGIN "Skip aws_key_management - ${msg}")
  ADD_FEATURE_INFO(AWS_KEY_MANAGEMENT "OFF" "AWS Encryption Key Management Plugin")
  RETURN()
ENDMACRO()
SET(CMAKE_CXX_STANDARD 11)

IF(NOT NOT_FOR_DISTRIBUTION)
  SKIP_AWS_PLUGIN("AWS SDK has Apache 2.0 License which is not complatible with GPLv2. Set -DNOT_FOR_DISTRIBUTION=ON if you need this plugin")
ENDIF()

# This plugin needs recent C++ compilers (AWS C++ SDK header files are using C++11 features)
SET(CXX11_FLAGS)
SET(OLD_COMPILER_MSG "AWS SDK requires c++11 -capable compiler (minimal supported versions are g++ 4.8, clang 3.3, VS2103)")

IF(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
  EXECUTE_PROCESS(COMMAND ${CMAKE_CXX_COMPILER} -dumpversion OUTPUT_VARIABLE GCC_VERSION)
  IF (GCC_VERSION VERSION_LESS 4.8)
    SKIP_AWS_PLUGIN("${OLD_COMPILER_MSG}")
  ENDIF()
  SET(CXX11_FLAGS "-std=c++11")
ELSEIF (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  IF ((CMAKE_CXX_COMPILER_VERSION AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS  3.3) OR
     (CLANG_VERSION_STRING AND CLANG_VERSION_STRING VERSION_LESS  3.3))
    SKIP_AWS_PLUGIN("${OLD_COMPILER_MSG}")
  ENDIF()
  SET(CXX11_FLAGS "-stdlib=libc++")
ELSEIF(MSVC)
  IF (MSVC_VERSION LESS 1800)
    SKIP_AWS_PLUGIN("${OLD_COMPILER_MSG}")
  ENDIF()
ELSE()
  SKIP_AWS_PLUGIN("Compiler not supported by AWS C++ SDK")
ENDIF()

IF (NOT(WIN32 OR APPLE  OR (CMAKE_SYSTEM_NAME MATCHES "Linux")))
  SKIP_AWS_PLUGIN("OS unsupported by AWS SDK")
ENDIF()



FIND_LIBRARY(AWS_CPP_SDK_CORE NAMES aws-cpp-sdk-core PATH_SUFFIXES  "${SDK_INSTALL_BINARY_PREFIX}")
FIND_LIBRARY(AWS_CPP_SDK_KMS NAMES aws-cpp-sdk-kms PATH_SUFFIXES  "${SDK_INSTALL_BINARY_PREFIX}")
FIND_PATH(AWS_CPP_SDK_INCLUDE_DIR NAMES aws/kms/KMSClient.h)

IF(AWS_CPP_SDK_CORE AND AWS_CPP_SDK_KMS AND AWS_CPP_SDK_INCLUDE_DIR)
  # AWS C++ SDK installed
  INCLUDE_DIRECTORIES(${AWS_CPP_SDK_INCLUDE_DIR})
  SET(AWS_SDK_LIBS ${AWS_CPP_SDK_CORE} ${AWS_CPP_SDK_KMS})
ELSE()
  OPTION(AWS_SDK_EXTERNAL_PROJECT  "Allow download and build AWS C++ SDK" OFF)
  IF(NOT AWS_SDK_EXTERNAL_PROJECT)
    SKIP_AWS_PLUGIN("AWS_SDK_EXTERNAL_PROJECT is not set")
  ENDIF()
  # Build from source, using ExternalProject_Add
  # AWS C++ SDK requires cmake 2.8.12
  IF(CMAKE_VERSION VERSION_LESS "2.8.12")
    SKIP_AWS_PLUGIN("CMake is too old")
  ENDIF()
  FIND_PACKAGE(Git)
  IF(NOT GIT_FOUND)
    SKIP_AWS_PLUGIN("no GIT")
  ENDIF()
  INCLUDE(ExternalProject)
  IF(UNIX)
    FIND_PACKAGE(CURL)
    SET_PACKAGE_PROPERTIES(CURL PROPERTIES TYPE REQUIRED)
    IF(NOT CURL_FOUND)
      SKIP_AWS_PLUGIN("AWS C++ SDK requires libcurl development package")
    ENDIF()
    SET(PIC_FLAG -fPIC)
    FIND_PATH(UUID_INCLUDE_DIR uuid/uuid.h)
    IF(NOT UUID_INCLUDE_DIR)
      SKIP_AWS_PLUGIN("AWS C++ SDK requires uuid development package")
    ENDIF()
    IF(NOT APPLE)
      FIND_LIBRARY(UUID_LIBRARIES uuid)
      SET_PACKAGE_PROPERTIES(UUID_LIBRARIES PROPERTIES TYPE REQUIRED)
      IF(NOT UUID_LIBRARIES)
        SKIP_AWS_PLUGIN("AWS C++ SDK requires uuid development package")
      ENDIF()
      FIND_PACKAGE(OpenSSL)
      SET_PACKAGE_PROPERTIES(OpenSSL PROPERTIES TYPE REQUIRED)
      IF(NOT OPENSSL_FOUND)
        SKIP_AWS_PLUGIN("AWS C++ SDK requires openssl development package")
      ENDIF()
    ENDIF()
  ENDIF()
  IF(MSVC)
    SET(EXTRA_SDK_CMAKE_FLAGS -DCMAKE_CXX_FLAGS_DEBUGOPT="" -DCMAKE_EXE_LINKER_FLAGS_DEBUGOPT="" "-DCMAKE_CXX_FLAGS=/wd4530 /wd4577 /WX-")
  ENDIF()
  IF(CMAKE_CXX_COMPILER)
   SET(EXTRA_SDK_CMAKE_FLAGS ${EXTRA_SDK_CMAKE_FLAGS} -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
  ENDIF()

  SET(byproducts )
  # We do not need to build the whole SDK , just 2 of its libs
  set(AWS_SDK_LIBS aws-cpp-sdk-core aws-cpp-sdk-kms)
  FOREACH(lib ${AWS_SDK_LIBS})
    ADD_LIBRARY(${lib} STATIC IMPORTED GLOBAL)
    ADD_DEPENDENCIES(${lib} aws_sdk_cpp)
    SET(loc "${CMAKE_CURRENT_BINARY_DIR}/aws_sdk_cpp/lib/${CMAKE_STATIC_LIBRARY_PREFIX}${lib}${CMAKE_STATIC_LIBRARY_SUFFIX}")
    IF(CMAKE_VERSION VERSION_GREATER "3.1")
      SET(byproducts ${byproducts} BUILD_BYPRODUCTS ${loc})
    ENDIF()
    SET_TARGET_PROPERTIES(${lib} PROPERTIES IMPORTED_LOCATION ${loc})
  ENDFOREACH()

  # To be compatible with older cmake, we use older version of the SDK
  IF(CMAKE_VERSION LESS "3.0")
     SET(GIT_TAG "1.0.8")
  ELSE()
    SET(GIT_TAG "1.2.11")
  ENDIF()

  SET(AWS_SDK_PATCH_COMMAND )
  ExternalProject_Add(
    aws_sdk_cpp
    GIT_REPOSITORY "https://github.com/awslabs/aws-sdk-cpp.git"
    GIT_TAG ${GIT_TAG}
    UPDATE_COMMAND ""
    SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/aws-sdk-cpp"
    ${byproducts}
    CMAKE_ARGS
    -DBUILD_ONLY=kms
    -DBUILD_SHARED_LIBS=OFF
    -DFORCE_SHARED_CRT=OFF
    -DENABLE_TESTING=OFF
    "-DCMAKE_CXX_FLAGS_DEBUG=${CMAKE_CXX_FLAGS_DEBUG} ${PIC_FLAG}"
    "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=${CMAKE_CXX_FLAGS_RELWITHDEBINFO} ${PIC_FLAG}"
    "-DCMAKE_CXX_FLAGS_RELEASE=${CMAKE_CXX_FLAGS_RELEASE} ${PIC_FLAG}"
    "-DCMAKE_CXX_FLAGS_MINSIZEREL=${CMAKE_CXX_FLAGS_MINSIZEREL} ${PIC_FLAG}"
    ${EXTRA_SDK_CMAKE_FLAGS}
    -DCMAKE_INSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/aws_sdk_cpp
    -DCMAKE_INSTALL_LIBDIR=lib
    TEST_COMMAND ""
  )
  SET_TARGET_PROPERTIES(aws_sdk_cpp PROPERTIES EXCLUDE_FROM_ALL TRUE)

  IF(CMAKE_SYSTEM_NAME MATCHES "Linux")
    # Need whole-archive , otherwise static libraries are not linked
    SET(AWS_SDK_LIBS -Wl,--whole-archive ${AWS_SDK_LIBS} -Wl,--no-whole-archive)
  ENDIF()
  SET_TARGET_PROPERTIES(aws_sdk_cpp PROPERTIES EXCLUDE_FROM_ALL TRUE)
  INCLUDE_DIRECTORIES(${CMAKE_CURRENT_BINARY_DIR}/aws_sdk_cpp/include)
ENDIF()

ADD_DEFINITIONS(${SSL_DEFINES}) # Need to know whether openssl should be initialized
IF(CMAKE_VERSION GREATER "3.0")
 SET(CMAKE_CXX_STANDARD 11)
ELSE()
 SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}  ${CXX11_FLAGS}")
ENDIF()
IF(WIN32)
 SET(AWS_CPP_SDK_DEPENDENCIES bcrypt winhttp wininet userenv version)
ELSE()
 SET(AWS_CPP_SDK_DEPENDENCIES ${OPENSSL_LIBRARIES} ${CURL_LIBRARIES} ${UUID_LIBRARIES})
ENDIF()
MYSQL_ADD_PLUGIN(aws_key_management aws_key_management_plugin.cc
                 LINK_LIBRARIES ${AWS_SDK_LIBS} ${AWS_CPP_SDK_DEPENDENCIES}
                 COMPONENT aws-key-management)

IF (TARGET aws_key_management)
 SET(NON_DISTRIBUTABLE_WARNING "Apache 2.0" PARENT_SCOPE)
ENDIF()

ADD_FEATURE_INFO(AWS_KEY_MANAGEMENT "ON" "AWS Encryption Key Management Plugin")
