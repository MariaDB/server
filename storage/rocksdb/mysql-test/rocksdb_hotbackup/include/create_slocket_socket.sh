#!/bin/bash

src_data_dir="${MYSQLTEST_VARDIR}/mysqld.1/data/"
python -c "import socket as s; sock = s.socket(s.AF_UNIX); sock.bind('${src_data_dir}/slocket')"
