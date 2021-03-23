if [ -f /usr/lib/systemd/system/mariadb.service -a -x /usr/bin/systemctl ]; then
  systemd_conf=/etc/systemd/system/mariadb.service.d/migrated-from-my.cnf-settings.conf
  if [ -x %{_bindir}/mariadb-service-convert -a ! -f "${systemd_conf}" ]; then
    # Either fresh install or upgrade non-systemd -> systemd
    mkdir -p /etc/systemd/system/mariadb.service.d
    %{_bindir}/mariadb-service-convert > "${systemd_conf}"
    # Make sure old possibly non-systemd instance is down
    if [ $1 = 2 ]; then
      SYSTEMCTL_SKIP_REDIRECT=1 %{_sysconfdir}/init.d/mysql stop >/dev/null 2>&1 || :
      systemctl start mariadb >/dev/null 2>&1 || :
    fi
    systemctl enable mariadb.service >/dev/null 2>&1 || :
  fi
fi

# Make MySQL start/shutdown automatically when the machine does it.
if [ $1 = 1 ] ; then
  if [ -x /usr/bin/systemctl ] ; then
          /usr/bin/systemctl daemon-reload >/dev/null 2>&1 || :
          /usr/bin/systemctl preset mariadb.service >/dev/null 2>&1 || :
  elif [ -x /sbin/chkconfig ] ; then
          /sbin/chkconfig --add mysql
  fi

  basedir=`%{_bindir}/my_print_defaults --mysqld|sed -ne 's/^--basedir=//p'|tail -1`
  if [ -z "$basedir" ] ; then
    basedir=%{mysqlbasedir}
  fi

  datadir=`%{_bindir}/my_print_defaults --mysqld|sed -ne 's/^--datadir=//p'|tail -1`
  if [ -z "$datadir" ] ; then
    datadir=%{mysqldatadir}
  else
    # datadir may be relative to a basedir!
    if ! expr $datadir : / > /dev/null; then
      datadir=$basedir/$datadir
    fi
  fi

  # Create a MySQL user and group. Do not report any problems if it already
  # exists.
  groupadd -r %{mysqld_group} 2> /dev/null || true
  useradd -M -r --home $datadir --shell /sbin/nologin --comment "MySQL server" --gid %{mysqld_group} %{mysqld_user} 2> /dev/null || true
  # The user may already exist, make sure it has the proper group nevertheless (BUG#12823)
  usermod --gid %{mysqld_group} %{mysqld_user} 2> /dev/null || true

  # Temporary Workaround for MDEV-11386 - will be corrected in Advance Toolchain 10.0-3 and 8.0-8
  for ldconfig in /opt/at*/sbin/ldconfig; do
     test -x $ldconfig && $ldconfig
  done

  # Change permissions so that the user that will run the MySQL daemon
  # owns all database files.
  chown -R -f %{mysqld_user}:%{mysqld_group} $datadir

  if [ ! -e $datadir/mysql ]; then
    # Create data directory
    mkdir -p $datadir

    # Initiate databases
    %{_bindir}/mysql_install_db --rpm --user=%{mysqld_user}
  fi

  # Change permissions again to fix any new files.
  chown -R %{mysqld_user}:%{mysqld_group} $datadir

  # Fix permissions for the permission database so that only the user
  # can read them.
  chmod -R og-rw $datadir/mysql
fi

# Set the correct filesystem ownership for the PAM v2 plugin
chown %{mysqld_user} /usr/lib*/mysql/plugin/auth_pam_tool_dir

# install SELinux files - but don't override existing ones
SETARGETDIR=/etc/selinux/targeted/src/policy
SEDOMPROG=$SETARGETDIR/domains/program
SECONPROG=$SETARGETDIR/file_contexts/program

if [ -x /usr/sbin/semodule ] ; then
  /usr/sbin/semodule -i /usr/share/mysql/policy/selinux/mariadb.pp
fi

if [ -x /sbin/restorecon ] ; then
	/sbin/restorecon -R /var/lib/mysql
fi

