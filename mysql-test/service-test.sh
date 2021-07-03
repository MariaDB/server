#!/bin/bash
#
# Test MariaDB systemd and SysV init etc service files and configuration to
# verify it works as is should on the platform this test is run on.
#
# Related: https://jira.mariadb.org/browse/MDEV-15526
#
# Launch this with e.g.
#     docker run --rm -it --name simple-test -v "${PWD}:/test" -w /test --shm-size=1G debian:sid ./server/mysql-test/service-test.sh
#
# or
#     docker run -d --name restart-test --privileged -v "${PWD}:/test" -w /test --shm-size=1G -v /sys/fs/cgroup:/sys/fs/cgroup:ro jrei/systemd-debian:sid
#     docker exec -it restart-test ./server/mysql-test/service-test.sh
#     docker exec -it restart-test reboot
#     docker start restart-test
#     docker exec -it restart-test ./server/mysql-test/service-test.sh
#     docker stop restart-test && docker rm restart-test
#
# or
#     vagrant init ubuntu/bionic64
#     vagrant up
#     vagrant ssh -t -c 'sudo /vagrant/service-test.sh'
#     vagrant ssh -c 'sudo reboot'
#     vagrant ssh -t -c 'sudo /vagrant/service-test.sh'
#     vagrant destroy
#
# Expected results:
# - in a systemd system, systemctl will control both init and systemd
# - on a init system, with or without systemctl shim, the update-rc.d command
#   will control both rc.* status and /etc/systemd/*

# This script should not have any uncatched non-zero exit codes
set -e

function check_service_status {
  SERVICE_STATUS="" # Reset empty variable

  if find /etc/rc* | grep -e mariadb -e mysql | grep --quiet -F S
  then
    echo "--> rc.d / init: enabled, starts on boot"
    SERVICE_STATUS+="sysvinit=1;"
  else
    echo "--> rc.d / init: disabled"
    SERVICE_STATUS+="sysvinit=0;"
  fi

  if find /etc/systemd/system/*target* -ls | grep --quiet -e mariadb -e mysql
  then
    echo "--> systemd: enabled, starts on boot"
    SERVICE_STATUS+="systemd=1;"
  else
    echo "--> systemd: disabled"
    SERVICE_STATUS+="systemd=0;"
  fi

  if find /etc/systemd/ /lib/systemd/ -ls | grep -e mariadb -e mysql | grep --quiet dev/null
  then
    echo "--> systemd: masked, cannot be enabled nor started"
    SERVICE_STATUS="masked=1;"
  fi

  if [ -n "$1" ]
  then
    if [ "$SERVICE_STATUS" != "$1" ]
    then
      echo "ERROR! service status '$SERVICE_STATUS' did not match expected '$1'"
      exit 1
    fi
  fi
}

if ! which apt-get > /dev/null
then
  echo "ERROR! This script has only been developed/tested on Debian/Ubuntu"
  exit 1
fi

# Prime apt-get cache so later commands work
apt-get update

# Enable policy-rc.d so invoke-rc.d works in package maintainer files
if [ -f /usr/sbin/policy-rc.d ] && grep --quiet 101 /usr/sbin/policy-rc.d
then
  echo "The update-rc.d exists but is disabled. Enabling for tests to proceed."
  sed -i "s/101/0/g" -i /usr/sbin/policy-rc.d
fi

# Install MariaDB via local .deb files if exists
for _ in mariab-server*.deb
do
  apt-get install -y ./*.deb
done

# Install MariaDB from repositories if not already installed by now
if ! dpkg --get-selections | grep --quiet mariadb-server
then
  apt-get install -y mariadb-server
fi

# Verify installation of MariaDB built in this commit
dpkg -l | grep -iE 'maria|mysql|galera' # List installed
mariadb --version # Client version

# From this point onwards, show all commands to help debug failures
set -x

# Check service status with empty PAGER to avoid running 'less'
export PAGER=''
service mysql status
find /etc/init.d/ -name 'm*' -ls
find /etc/rc* | grep -e mariadb -e mysql

# If systemd not present, install the non-systemd systemctl shim
if ! which systemctl > /dev/null
then
  apt-get install -y systemctl
fi

systemctl status mariadb
systemctl status mysql
systemctl status mysqld
systemctl list-units | grep -e mariadb -e mysql
systemctl list-unit-files | grep -e mysql -e mariadb
find /etc/systemd/ /lib/systemd/ -ls | grep -e mariadb -e mysql

service mysql start
mariadb --skip-column-names -e "select @@version, @@version_comment" # Show version
echo 'SHOW DATABASES;' | mariadb # List databases
mariadb -e "create database test; use test; create table t(a int primary key) engine=innodb; insert into t values (1); select * from t; drop table t; drop database test;" # Test InnoDB works
service mysql stop

# Hide commands for the rest of the script make output more readable
set +x

echo "Status after installation.."
check_service_status "sysvinit=1;systemd=1;"
echo

echo "Disabling service in rc.d..."
update-rc.d mariadb disable
check_service_status "sysvinit=0;systemd=0;"
echo

echo "Enabling service in rc.d..."
update-rc.d mariadb enable
check_service_status "sysvinit=1;systemd=1;"
echo

echo "Disabling service in systemd..."
systemctl disable mariadb
check_service_status "sysvinit=0;systemd=0;"
echo

echo "Enabling service in systemd..."
systemctl enable mariadb
check_service_status "sysvinit=1;systemd=1;"
echo

echo "Disabling service in systemd with name 'mysql', not synced..."
systemctl disable mysql
check_service_status "sysvinit=1;systemd=0;"
echo

echo "Enabling service in systemd with name 'mysql'..."
systemctl enable mysql
# @TODO: this adds /etc/rc.*/S01mysql symlinks
check_service_status "sysvinit=1;systemd=1;"
echo

echo "Masking service in systemd..."
systemctl mask mariadb
check_service_status "masked=1;"

echo "Unmasking service in systemd..."
systemctl unmask mariadb
check_service_status "sysvinit=1;systemd=1;"

echo "Disabling service in systemd..."
systemctl disable mariadb
check_service_status "sysvinit=0;systemd=0;"
echo

echo "Enabling service in systemd..."
systemctl enable mariadb
check_service_status "sysvinit=1;systemd=1;"
echo

# @TODO: There is no /etc/init.d/msyqld so disabling mysqld in systemd with
# 'systemctl disable mysqld' does not trigger /lib/systemd/systemd-sysv-install
# for service name 'mysqld' and thus disabling the sysvinit service mariadb service
# stays enabled:
#   Synchronizing state of mariadb.service with SysV service script with /lib/systemd/systemd-sysv-install.
#   Executing: /lib/systemd/systemd-sysv-install disable mariadb

echo "Disabling service in systemd..."
systemctl disable mysqld
check_service_status "sysvinit=1;systemd=0;"
echo

echo "Enabling service in systemd..."
systemctl enable mysqld
check_service_status "sysvinit=1;systemd=1;"
echo

# NOTE! Script /usr/sbin/service is just a wrapper for init or systemctl,
# no need to test it separately. While invoke-rc.d is primairly a sysvinit tool,
# it also runs systemctl commands under the hood if systemd is detected.

# @TODO Test also service dependencies (Requires/Wants)
# https://www.freedesktop.org/software/systemd/man/systemd.unit.html#Wants=
cat > /lib/systemd/system/mariadbdependentservice.service <<EOF
[Unit]
Requires=mariadb.service

[Service]
ExecStart=/bin/sleep 3600
EOF

cat > /lib/systemd/system/mysqldependentservice.service <<EOF
[Unit]
Requires=mysql.service

[Service]
ExecStart=/bin/sleep 3600
EOF

systemctl daemon-reload
systemctl stop mariadb
sleep 5
systemctl start mariadbdependentservice
systemctl status mariadbdependentservice
systemctl stop mariadb
sleep 5
systemctl start mysqldependentservice
systemctl status mysqldependentservice
