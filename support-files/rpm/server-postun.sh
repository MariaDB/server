if [ $1 -ge 1 ]; then
  # only restart the server if it was alredy running
  if [ -x /usr/bin/systemctl ] ; then
    /usr/bin/systemctl daemon-reload > /dev/null 2>&1
    /usr/bin/systemctl try-restart mariadb.service > /dev/null 2>&1
  elif %{_sysconfdir}/init.d/mysql status > /dev/null 2>&1; then
    %{_sysconfdir}/init.d/mysql restart
  fi
fi

if [ $1 = 0 ] ; then
  if [ -x /usr/bin/systemctl ] ; then
    /usr/bin/systemctl daemon-reload > /dev/null 2>&1
  fi
fi

