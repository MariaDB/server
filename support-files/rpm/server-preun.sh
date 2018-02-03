%if %{with init_systemd}
%systemd_preun %{daemon_name}.service
%endif
%if %{with init_sysv}
if [ $1 = 0 ]; then
  # Stop MySQL before uninstalling it
  if [ -x /sbin/service ]; then
    /sbin/service %{daemon_name} stop >/dev/null 2>&1
  elif [ -x %{_sysconfdir}/init.d/%{daemon_name} ] ; then
    %{_sysconfdir}/init.d/%{daemon_name} stop > /dev/null
  fi
  if [ -x /sbin/chkconfig ] ; then
    /sbin/chkconfig --del ${daemon_name}
  fi
fi
%endif

# We do not remove the mysql user since it may still own a lot of
# database files.

