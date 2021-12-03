if [ -r %{restart_flag} ] ; then
  rm %{restart_flag}
  # only restart the server if it was already running
  if [ -x /usr/bin/systemctl ] ; then
    /usr/bin/systemctl daemon-reload > /dev/null 2>&1
    if /usr/bin/systemctl is-active mysql; then
      /usr/bin/systemctl restart mysql > /dev/null 2>&1
    else
      /usr/bin/systemctl try-restart mariadb.service > /dev/null 2>&1
    fi
  # not a systemd-enabled environment, use SysV startup script
  elif %{_sysconfdir}/init.d/mysql status > /dev/null 2>&1; then
    %{_sysconfdir}/init.d/mysql restart > /dev/null 2>&1
  fi
fi
