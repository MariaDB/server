if [ $1 -ge 1 ]; then
  # request the server restart
  mkdir -p %{restart_flag_dir}
  echo > %{restart_flag}
fi

if [ $1 = 0 ] ; then
  if [ -x /usr/bin/systemctl ] ; then
    /usr/bin/systemctl daemon-reload > /dev/null 2>&1
  fi
fi

