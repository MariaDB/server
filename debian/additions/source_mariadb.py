'''apport package hook for mariadb

(c) 2009 Canonical Ltd.
Author: Mathias Gug <mathias.gug@canonical.com>
'''

from __future__ import print_function, unicode_literals
import os, os.path

from apport.hookutils import *

def _add_my_conf_files(report, filename):
    key = 'MySQLConf' + path_to_key(filename)
    report[key] = ""
    for line in read_file(filename).split('\n'):
        try:
            if 'password' in line.split('=')[0]:
                line = "%s = @@APPORTREPLACED@@" % (line.split('=')[0])
            report[key] += line + '\n'
        except IndexError:
            continue

def add_info(report):
    attach_conffiles(report, 'mariadb-server', conffiles=None)
    key = 'Logs' + path_to_key('/var/log/daemon.log')
    report[key] = ""
    for line in read_file('/var/log/daemon.log').split('\n'):
        try:
            if 'mariadbd' in line.split()[4]:
                report[key] += line + '\n'
        except IndexError:
            continue
    if os.path.exists('/var/log/mysql/error.log'):
        key = 'Logs' + path_to_key('/var/log/mysql/error.log')
        report[key] = ""
        for line in read_file('/var/log/mysql/error.log').split('\n'):
            report[key] += line + '\n'
    attach_mac_events(report, '/usr/sbin/mariadbd')
    attach_file(report,'/etc/apparmor.d/usr.sbin.mariadbd')
    _add_my_conf_files(report, '/etc/mysql/mariadb.cnf')
    for f in os.listdir('/etc/mysql/conf.d'):
        _add_my_conf_files(report, os.path.join('/etc/mysql/conf.d', f))
    for f in os.listdir('/etc/mysql/mariadb.conf.d'):
        _add_my_conf_files(report, os.path.join('/etc/mysql/mariadb.conf.d', f))
    try:
        report['MySQLVarLibDirListing'] = str(os.listdir('/var/lib/mysql'))
    except OSError:
        report['MySQLVarLibDirListing'] = str(False)

if __name__ == '__main__':
    report = {}
    add_info(report)
    for key in report:
        print('%s: %s' % (key, report[key].split('\n', 1)[0]))
