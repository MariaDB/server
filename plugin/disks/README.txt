Information Schema Disks
------------------------
This is a proof-of-concept information schema plugin that allows the
disk space situation to be monitored. When installed, it can be used
as follows:

  > select * from information_schema.disks;
  +-----------+-----------------------+-----------+----------+-----------+
  | Disk      | Path                  | Total     | Used     | Available |
  +-----------+-----------------------+-----------+----------+-----------+
  | /dev/sda3 | /                     |  47929956 | 30666304 |  14805864 |
  | /dev/sda1 | /boot/efi             |    191551 |     3461 |    188090 |
  | /dev/sda4 | /home                 | 174679768 | 80335392 |  85448120 |
  | /dev/sdb1 | /mnt/hdd              | 961301832 |    83764 | 912363644 |
  | /dev/sdb1 | /home/wikman/Music    | 961301832 |    83764 | 912363644 |
  | /dev/sdb1 | /home/wikman/Videos   | 961301832 |    83764 | 912363644 |
  | /dev/sdb1 | /home/wikman/hdd      | 961301832 |    83764 | 912363644 |
  | /dev/sdb1 | /home/wikman/Pictures | 961301832 |    83764 | 912363644 |
  | /dev/sda3 | /var/lib/docker/aufs  |  47929956 | 30666304 |  14805864 |
  +-----------+-----------------------+-----------+----------+-----------+
  9 rows in set (0.00 sec)

- 'Disk' is the name of the disk itself.
- 'Path' is the mount point of the disk.
- 'Total' is the total space in KiB.
- 'Used' is the used amount of space in KiB, and
- 'Available' is the amount of space in KiB available to non-root users.

Note that as the amount of space available to root may be more that what
is available to non-root users, 'available' + 'used' may be less than 'total'.

All paths to which a particular disk has been mounted are reported. The
rationale is that someone might want to take different action e.g. depending
on which disk is relevant for a particular path. This leads to the same disk
being reported multiple times. An alternative to this would be to have two
tables; disks and mounts.

  > select * from information_schema.disks;
  +-----------+-----------+----------+-----------+
  | Disk      | Total     | Used     | Available |
  +-----------+-----------+----------+-----------+
  | /dev/sda3 |  47929956 | 30666304 |  14805864 |
  | /dev/sda1 |    191551 |     3461 |    188090 |
  | /dev/sda4 | 174679768 | 80335392 |  85448120 |
  | /dev/sdb1 | 961301832 |    83764 | 912363644 |
  +-----------+-----------+----------+-----------+

  > select * from information_schema.mounts;
  +-----------------------+-----------+
  | Path                  | Disk      |
  +-----------------------+-----------+
  | /                     | /dev/sda3 |
  | /boot/efi             | /dev/sda1 |
  | /home                 | /dev/sda4 |
  | /mnt/hdd              | /dev/sdb1 |
  | /home/wikman/Music    | /dev/sdb1 |
  ...


Installation
------------

- Use "install plugin" or "install soname" command:

  MariaDB [(none)]> install plugin disks soname 'disks.so';

  or

  MariaDB [(none)]> install soname 'disks.so';

Usage
-----
The plugin appears as the table 'disks' in 'information_schema'.

  MariaDB [(none)]> select * from information_schema.disks;
  +-----------+-----------------------+-----------+----------+-----------+
  | Disk      | Path                  | Total     | Used     | Available |
  +-----------+-----------------------+-----------+----------+-----------+
  | /dev/sda3 | /                     |  47929956 | 30666308 |  14805860 |
  | /dev/sda1 | /boot/efi             |    191551 |     3461 |    188090 |
  | /dev/sda4 | /home                 | 174679768 | 80348148 |  85435364 |
  | /dev/sdb1 | /mnt/hdd              | 961301832 |    83764 | 912363644 |
  | /dev/sdb1 | /home/wikman/Music    | 961301832 |    83764 | 912363644 |
  | /dev/sdb1 | /home/wikman/Videos   | 961301832 |    83764 | 912363644 |
  ...

