#!/bin/bash
#
# This script purpose is to check if MariaDB
# configuration has problematic configurations
# and make more reasonable error messages than
# it does now
#
# To make this script work you need:
#   * my_print_defaults from MariaDB package
#   * systemctl from systemd package
#
# If you want to get rid of this checking then
# you have add service file or profile.d enviromental
# variable (with value = 1):
# MARIADB_DO_NOT_PRECHECK=1
#
# And if you don't wish to use systemd then add
# variable:
# MARIADB_DO_NOT_SYSTEMCTL_CHECK=1
#
# Example add file /etc/systemd/system/mariadb.service.d/nocheck.conf
# [Service]
# Environment=MARIADB_DO_NOT_PRECHECK=1

if [ -n "${MARIADB_DO_NOT_PRECHECK}" ]
then
    exit 0
fi

MY_PRINT_DEFAULTS=""

if [ -z "${MY_PRINT_DEFAULTS}" ]
then
    if [ -x "@bindir@/my_print_defaults" ]
    then
        MY_PRINT_DEFAULTS="@bindir@/my_print_defaults"
    elif my_print_defaults -n
    then
        MY_PRINT_DEFAULTS=my_print_defaults
    fi
else
    if ! "${MY_PRINT_DEFAULTS}" -n
    then
        echo "Can't find tool: '$MY_PRINT_DEFAULTS' in path."
        echo "You need to check set MY_PRINT_DEFAULTS to the correct my_print_defaults executable"
        echo "or set environment variable MARIADB_DO_NOT_PRECHECK=1 to disable this check"
        exit 1
    fi
fi

if [ -z "${MY_PRINT_DEFAULTS}" ]
then
    echo "Can't find tool my_print_defaults in expected locations."
    echo "You can set the environment variable MY_PRINT_DEFAULTS to the location"
    exit 1
fi

SERVICE_NAME=${1:-mariadb.service}
if [ $# -ge 1 ]
then
    shift
fi
MARIADB_PROTECTSYSTEM=""
MARIADB_PROTECTHOME=""

# If ENV variable MARIADB_DO_NOT_SYSTEMCTL_CHECK is set
# then do not use system stuff
if [ -z "${MARIADB_DO_NOT_SYSTEMCTL_CHECK}" ]
then
    # Check with systemctl cat mariadb*.service
    # if user has already altered service file to
    # allow writing to there locations

    # Check service file should we even check
    # anything. If ProtectHome and ProtectSystem
    # are disabled then there is not issues
    MARIADB_PROTECTSYSTEM=$(systemctl cat "${SERVICE_NAME}" | grep -e "^ProtectSystem=no")
    MARIADB_PROTECTHOME=$(systemctl cat "${SERVICE_NAME}" | grep -e "^ProtectHome=no")

    # If both are set then just exit as everything is fine
    if [ -n "${MARIADB_PROTECTSYSTEM}" ] && [ -n "${MARIADB_PROTECTHOME}" ]
    then
        exit 0
    fi
fi

#
# Check with my_print_defaults that any
# path in configuration is not pointing somewhere
# we can't read/write or it is invisible
#
# Additional arguments like --defaults-(extra-file,file,group-suffix)
# can be added to the script to match the server configuration.

"${MY_PRINT_DEFAULTS}" "$@" --mysqld | while read -r setting
do
    conf_var=${setting%%=*}
    conf_var=${conf_var#--}
    conf_val=${setting#*=}
    conf_problem=""
    conf_read_x_paths="Write"

    # Read only conf_vars
    # https://github.com/MariaDB/server/pull/1906#discussion_r714422253
    case "$conf_var" in
        basedir) ;&
        plugin[-_]dir) ;&
        file[-_]key[-_]management[-_]filename) ;&
        secure[-_]file[-_]priv)
            if [ ! -r "${conf_val}" ]
            then
                echo "server variable $conf_var location $conf_val is not readable"
                conf_read_x_paths="Only"
            else
                continue
            fi
            ;;
        *)
            if [ "${conf_val:1:1}" = '/' ] && [ ! -w "$conf_val" ]
            then
                echo "server variable $conf_var location $conf_val is not writable"
            else
                continue
            fi
            ;;
    esac
    # These are the places that systemd
    # is prevented currently to write or access
    conf_problem=""
    case "$conf_val" in
            # ProtectSystem prevents writes to /usr, /boot and /efi
        /usr|/usr/*|/boot|/boot/*|/efi|/efi/*)
            if [ -z "${MARIADB_PROTECTSYSTEM}" ]
            then
                conf_problem="ProtectSystem"
            fi
            ;;
            # ProtectHome prevents access to /home, /root and /run/user
        /home|/home/*|/root|/root/*|/run/user|/run/user*)
            if [ -z "${MARIADB_PROTECTHOME}" ]
            then
                conf_problem="ProtectHome"
            fi
            ;;
    esac

    if [ -n "${conf_problem}" ]
    then
        echo "Your MariaDB server variable '${conf_var}' points to '${conf_val}' location and currently MariaDB systemd service file prevents that."
        echo "To solve configuration problem you should 'systemctl edit $SERVICE_NAME'"

        echo "In this editor, under a [Service] section, recommend configure Read${conf_read_x_paths}Paths=${conf_val}."
        echo "Alternately disable protection with ${conf_problem}=No"
        echo -e "\nWithout modification of your '${conf_var}' configuration or suggested systemd service file changes"
        echo -e "it's most likely your MariaDB database won't start!\n"
        echo "Read more from systemd.exec(5) man page or https://www.freedesktop.org/software/systemd/man/systemd.exec.html"
        exit 1
    fi
done
