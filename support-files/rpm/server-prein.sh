# Create a MariaDB user and group. Do not report any problems if it already exists.
groupadd -r %{mysqld_group} 2> /dev/null || true
useradd -M -r --home %{mysqldatadir} --shell /sbin/nologin --comment "MariaDB server" --gid %{mysqld_group} %{mysqld_user} 2> /dev/null || true
# The user may already exist, make sure it has the proper group nevertheless (BUG#12823)
usermod --gid %{mysqld_group} %{mysqld_user} 2> /dev/null || true
