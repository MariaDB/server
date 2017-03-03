#!/bin/bash

# This is a template fgenerator or repeating an identical suite of tests for each backing table storage engine
# It generates a set of .test files for the following, for example:
#   general-myisam.test
#   general-memory.test
#   general-innodb.test
#   (etc)
#
# We generate these files, because as a general rule the coverage should be identical per backing table engine
# but we might want to pick out and re-reun for an individual backing table engine
# otherwise we could use an MTR loop instead.

# This is intended to be used by a maintainer; i.e. the resulting .test files are still committed to git.

# Note on known storage engines:
# See https://mariadb.com/kb/en/information-schema-engines-table/ for a full list
# CSV - doesn't work with OQGraph, attempting to create backing table gives 'failed: 1069: Too many keys specified; max 0 keys allowed'
# BLACKHOLE - makes no sense... but we should make sure it doesnt crash
# FEDERATED, ARCHIVE - consider later 

ENGINES="MyISAM MEMORY Aria"

for ENGINE in $ENGINES ; do
  cat > general-$ENGINE.test <<EOF
# This is a maintainer generated file. Generated at `date`.
--let \$oqgraph_use_table_type= $ENGINE
--source general.inc
EOF
done

# These engines need an extra check to see if thy are compiled
# Note, including innodb will also test xtradb
ENGINES2="innodb"
for ENGINE in $ENGINES2 ; do
  cat > general-$ENGINE.test <<EOF
# This is a maintainer generated file. Generated at `date`.
-- source include/have_$ENGINE.inc
--let \$oqgraph_use_table_type= $ENGINE
--source general.inc
EOF
done

# Generate a script to rerun the test suite as well
# Intended to be run from build as ../storage/oqgraph/mysql-test/oqgraph/maintainer-general-record.sh

MGFILE=maintainer-general-record.sh
echo '#!/bin/sh' > $MGFILE
echo '# This is a maintainer generated file. Generated at '`date`'.' >> $MGFILE
for ENGINE in $ENGINES $ENGINES2 ; do
  echo mysql-test/mysql-test-run --record oqgraph.general-$ENGINE >> $MGFILE
done
  
