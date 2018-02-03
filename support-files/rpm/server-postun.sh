%if %{with init_systemd}
%systemd_postun_with_restart %{daemon_name}.service
%endif
%if %{with init_sysv}
if [ $1 -ge 1 ]; then
  if [ -x /sbin/servce ] ; then
    /sbin/service %{daemon_name} condrestart >/dev/null 2>&1 || :
  elif [ -x %{_sysconfdir}/init.d/%{daemon_name} ] ; then
    # only restart the server if it was alredy running
    if %{_sysconfdir}/init.d/%{daemon_name} status > /dev/null 2>&1; then
      %{_sysconfdir}/init.d/%{daemon_name} restart
    fi
fi
%endif
