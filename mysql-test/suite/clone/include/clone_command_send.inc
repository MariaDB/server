## Send Clone command

# These variables have to be set before sourcing this file. Currently we used
# to test both local and remote clone.
#
# 1. Clone command is expected to return error
# --let clone_err= <error number>
#
#

# Execute Local Clone command
--replace_result $CLONE_DATADIR CLONE_DATADIR
--send_eval CLONE LOCAL DATA DIRECTORY = '$CLONE_DATADIR'
