SETARGETDIR=/etc/selinux/targeted/src/policy
SEDOMPROG=$SETARGETDIR/domains/program
SECONPROG=$SETARGETDIR/file_contexts/program

if [ -x /usr/sbin/semodule ] ; then
  /usr/sbin/semodule -i /usr/share/mysql/policy/selinux/mariadb-plugin-cracklib-password-check.pp
fi

