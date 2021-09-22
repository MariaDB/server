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
# MARIADB_DO_NOT_EXECSTARTPRE_CHECK=1
#
# And if you don't like to use systemd then add
# variable
# MARIADB_DO_NOT_EXECSTARTPRE_SYSTEMCTL=1
#
# Example add file /etc/systemd/system/mariadb.service.d/nocheck.conf
# [Service]
# Environment=MARIADB_DO_NOT_EXECSTARTPRE_CHECK=1
#

# If ENV variable MARIADB_DO_NOT_EXECSTARTPRE_CHECK is set
# Then just exit
if [ -n "${MARIADB_DO_NOT_EXECSTARTPRE_CHECK}" ]
then
    exit 0
fi

MY_PRINT_DEFAULTS=""

if [ -x "@bindir@/my_print_defaults" ]
then
   MY_PRINT_DEFAULTS="@bindir@/my_print_defaults"
fi

if [ -z "${MY_PRINT_DEFAULTS}" ]
then
    echo "Can't find tool: 'my_print_defaults' in path."
    echo "You need to check your system settings or set enviromental"
    echo "variable: MARIADB_DO_NOT_EXECSTARTPRE_CHECK=1 to prevent this error"
    exit 1
fi

MARIADB_PROTECTSYSTEM=""
MARIADB_PROTECTHOME=""

# If ENV variable MARIADB_DO_NOT_EXECSTARTPRE_SYSTEMCTL is set
# then do not use system stuff
if [ -z "${MARIADB_DO_NOT_EXECSTARTPRE_SYSTEMCTL}" ]
then
    # Check service file should we even check
    # anything. If ProtectHome and ProtectSystem
    # are disabled then there is not issues
    MARIADB_PROTECTSYSTEM=$(systemctl cat mariadb.service | grep -e "^ProtectSystem=no")
    MARIADB_PROTECTHOME=$(systemctl cat mariadb.service | grep -e "^ProtectHome=no")

    # If both are set then just exit as everything is fine
    if [ -n "${MARIADB_PROTECTSYSTEM}" ] && [ -n "${MARIADB_PROTECTHOME}" ]
    then
        exit 0
    fi
fi


#
# Check with my_print_defaults that any
# path in configuration is not pointing somewhere
# we can't write or it is invisible
#
# Check with systemctl cat mariadb*.service
# if user has already altered service file to
# allow writing to there locations
#
my_print_defaults --mysqld | while read -r setting
do
    conf_var=${setting%%=*}
    conf_var=${conf_var#--}
    conf_val=${setting#*=}
    conf_problem=""

    # These are the places that systemd
    # is prevented currently to write or access
    case "$conf_val" in
            # ProtectSystem prevents writes to /usr, /boot and /efi
        /usr|/usr/*|/boot|/boot/*|/efi|/efi/*)
            if [ -z "${MARIADB_PROTECTSYSTEM}" ]
            then
                conf_problem="ProtectSystem"
            else
                conf_problem=""
            fi
            ;;
            # ProtectHome prevents access to /home, /root and /run/user
        /home|/home/*|/root|/root/*|/run/user|/run/user*)
            if [ -z "${MARIADB_PROTECTHOME}" ]
            then
                conf_problem="ProtectHome"
            else
                conf_problem=""
            fi
            ;;
    esac

    if [ -n "${conf_problem}" ]
    then

        echo "Your MariaDB server variable '${conf_var}' points to '${conf_val}' location and currently MariaDB systemd service file prevents that."
        echo "To solve configuration problem you should create '/etc/systemd/system/mariadb.service.d/overwrite.conf'"
        echo "In that file, under a [Service] section, recommend configure ReadWritePaths=${conf_val}"
        echo "Alternately disable protection with ${conf_problem}=No"
        echo -e "\nWithout modification of your '${conf_var}' configuration or suggested systemd service file changes"
        echo -e "it's most likely your MariaDB database won't start!\n"
        echo "Read more from systemd.exec(5) man page or https://www.freedesktop.org/software/systemd/man/systemd.exec.html"
        return 1
    fi
done
