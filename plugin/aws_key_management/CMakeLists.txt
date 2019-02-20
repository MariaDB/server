INCLUDE(aws_sdk)
CHECK_AWS_SDK(HAVE_AWS_SDK REASON)
IF(NOT HAVE_AWS_SDK)
  MESSAGE_ONCE(AWS_KEY_MANAGEMENT_NO_AWS_SDK "Can't build aws_key_management - AWS SDK not available (${REASON})")
  RETURN()
ENDIF()
IF(NOT NOT_FOR_DISTRIBUTION)
  SKIP_AWS_PLUGIN("AWS SDK has Apache 2.0 License which is not complatible with GPLv2. Set -DNOT_FOR_DISTRIBUTION=ON if you need this plugin")
ENDIF()

MYSQL_ADD_PLUGIN(aws_key_management
  aws_key_management_plugin.cc
  COMPONENT aws-key-management)

IF(TARGET aws_key_management)
  SET(NON_DISTRIBUTABLE_WARNING "Apache 2.0" PARENT_SCOPE)
 USE_AWS_SDK_LIBS(aws_key_management kms)
ENDIF()
