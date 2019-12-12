if [ $1 = 0 ] ; then
        # Stop MySQL before uninstalling it
        # Don't start it automatically anymore
        if [ -x /usr/bin/systemctl ] ; then
                /usr/bin/systemctl stop mariadb.service > /dev/null 2>&1
                /usr/bin/systemctl disable mariadb.service > /dev/null 2>&1
        fi
        if [ -x %{_sysconfdir}/init.d/mysql ] ; then
                %{_sysconfdir}/init.d/mysql stop > /dev/null
                if [ -x /sbin/chkconfig ] ; then
                        /sbin/chkconfig --del mysql > /dev/null 2>&1 || :
                fi
        fi
fi

# We do not remove the mysql user since it may still own a lot of
# database files.

