## Clone command test

# These variables can to be set before sourcing this file. Currently we used
# to test both local and remote clone.
#
# 1. Clone command is expected to return error
# --let clone_err= <error number>
#
if ($clone_err != ER_PLUGIN_IS_NOT_LOADED) {
  if ($clone_throttle) {
    SET GLOBAL clone_max_data_bandwidth = 5;
  }
}

# Execute Local Clone command with error
if ($clone_err) {
  if ($clone_err == ER_FILE_EXISTS_ERROR) {
      --replace_result $MYSQL_TMP_DIR MYSQL_TMP_DIR
      --error $clone_err
      --eval CLONE LOCAL DATA DIRECTORY = '$CLONE_DATADIR'
    }

    if ($clone_err != ER_FILE_EXISTS_ERROR) {
      --replace_result $CLONE_DATADIR CLONE_DATADIR
      --error $clone_err
      --eval CLONE LOCAL DATA DIRECTORY = '$CLONE_DATADIR'
    }
}

# Execute Local Clone command
if (!$clone_err) {
  --replace_result $CLONE_DATADIR CLONE_DATADIR
  --eval CLONE LOCAL DATA DIRECTORY = '$CLONE_DATADIR'
}
