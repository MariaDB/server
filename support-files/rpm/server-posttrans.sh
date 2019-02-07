if [ -r %{restart_flag} ] ; then
  rm %{restart_flag}
  if [ -x /usr/bin/systemctl ] ; then
    /usr/bin/systemctl daemon-reload > /dev/null 2>&1
  fi

  # only restart the server if it was alredy running
  if %{_sysconfdir}/init.d/mysql status > /dev/null 2>&1; then
    %{_sysconfdir}/init.d/mysql restart
  fi
fi
