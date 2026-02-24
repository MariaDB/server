INCLUDE(aws_sdk)
CHECK_AWS_SDK("kms;core" HAVE_AWS_SDK REASON)
IF(NOT HAVE_AWS_SDK)
  MESSAGE_ONCE(AWS_KEY_MANAGEMENT_NO_AWS_SDK "Can't build aws_key_management - AWS SDK not available (${REASON})")
  IF(PLUGIN_AWS_KEY_MANAGEMENT MATCHES "^(STATIC|DYNAMIC)$")
    MESSAGE(FATAL_ERROR "Can't build reqired plugin aws_key_management: ${REASON}")
  ENDIF()
  ADD_FEATURE_INFO(AWS_KEY_MANAGEMENT "OFF" "AWS Encryption Key Management Plugin")
  RETURN()
ENDIF()

MYSQL_ADD_PLUGIN(aws_key_management
  aws_key_management_plugin.cc
  COMPONENT aws-key-management)

IF(TARGET aws_key_management)
  USE_AWS_SDK_LIBS(aws_key_management kms core)
  ADD_FEATURE_INFO(AWS_KEY_MANAGEMENT "ON" "AWS Encryption Key Management Plugin")
ENDIF()

