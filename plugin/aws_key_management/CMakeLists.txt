INCLUDE(aws_sdk)
CHECK_AWS_SDK(HAVE_AWS_SDK REASON)
IF(NOT HAVE_AWS_SDK)
  MESSAGE_ONCE(AWS_KEY_MANAGEMENT_NO_AWS_SDK "Can't build aws_key_management - AWS SDK not available (${REASON})")
  ADD_FEATURE_INFO(AWS_KEY_MANAGEMENT "OFF" "AWS Encryption Key Management Plugin")
  RETURN()
ENDIF()

MYSQL_ADD_PLUGIN(aws_key_management
  aws_key_management_plugin.cc
  COMPONENT aws-key-management)
set(AWS_SDK_CPP_LIB_DIR ${CMAKE_BINARY_DIR}/extra/aws_sdk/aws_sdk_cpp/lib)

target_link_libraries(aws_key_management
 -Wl,--start-group
  ${AWS_SDK_CPP_LIB_DIR}/libaws-c-common.a
  ${AWS_SDK_CPP_LIB_DIR}/libaws-c-io.a
  ${AWS_SDK_CPP_LIB_DIR}/libaws-c-http.a
  ${AWS_SDK_CPP_LIB_DIR}/libaws-c-auth.a
  ${AWS_SDK_CPP_LIB_DIR}/libaws-c-cal.a
  ${AWS_SDK_CPP_LIB_DIR}/libaws-c-mqtt.a
  ${AWS_SDK_CPP_LIB_DIR}/libaws-c-s3.a
  ${AWS_SDK_CPP_LIB_DIR}/libaws-c-event-stream.a
  ${AWS_SDK_CPP_LIB_DIR}/libaws-c-sdkutils.a
  ${AWS_SDK_CPP_LIB_DIR}/libaws-c-compression.a
  ${AWS_SDK_CPP_LIB_DIR}/libaws-checksums.a
  ${AWS_SDK_CPP_LIB_DIR}/libaws-crt-cpp.a
  ${AWS_SDK_CPP_LIB_DIR}/libaws-cpp-sdk-core.a
  ${AWS_SDK_CPP_LIB_DIR}/libaws-cpp-sdk-kms.a
  ${AWS_SDK_CPP_LIB_DIR}/libaws-cpp-sdk-logs.a
  ${AWS_SDK_CPP_LIB_DIR}/libaws-cpp-sdk-sts.a
  ${CMAKE_BINARY_DIR}/extra/aws_sdk/aws_sdk_cpp-prefix/src/aws_sdk_cpp-build/lib/libs2n.a
 -Wl,--end-group
  -lz
)


IF(TARGET aws_key_management)
 USE_AWS_SDK_LIBS(aws_key_management kms s3 sts logs)
ENDIF()

ADD_FEATURE_INFO(AWS_KEY_MANAGEMENT "ON" "AWS Encryption Key Management Plugin")
