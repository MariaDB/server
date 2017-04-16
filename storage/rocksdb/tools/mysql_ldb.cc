//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
#include <my_config.h>
#include "../rdb_comparator.h"
#include "rocksdb/ldb_tool.h"

int main(int argc, char **argv) {
  rocksdb::Options db_options;
  myrocks::Rdb_pk_comparator pk_comparator;
  db_options.comparator = &pk_comparator;

  rocksdb::LDBTool tool;
  tool.Run(argc, argv, db_options);
  return 0;
}
