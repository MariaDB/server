# ==== Usage ====
#
# let $test_engine  = [connect | innodb | mroonga | rocksdb];
# let $test_library = [bzip2 | lz4 | lz4hc | lzma | lzo | snappy | zstd];
# source test_library.inc;

--echo
--echo
--echo #
--echo # Test --use-compression=$test_library on storage engine "$test_engine"
--echo #

--echo # Check if library is loaded
let $library_loaded = `
    SELECT Variable_value AS ""
    FROM information_schema.global_status
    WHERE Variable_name = "Compression_loaded_$test_library"
`;

if($library_loaded != "ON"){
    skip Library "$test_library" is not loaded;
}

--echo # Start server with default options ($test_library enabled)

let $restart_parameters = --use-compression=$test_library;
source include/restart_mysqld.inc;

let $test_table_name = `SELECT CONCAT("$test_engine", "_compressed_", "$test_library")`;

disable_warnings;
eval DROP TABLE IF EXISTS $test_table_name;
enable_warnings;

# Create compressed table according to engine
if($test_engine == "connect"){
    eval INSTALL SONAME "$HA_CONNECT_SO";
    
    # Connect only supports BZip2
    if($test_library != "bzip2"){
        echo Error: engine "connect" only supports library "bzip2";
        exit;
    }

    #FIXME: Connect test currently fails
    let $table_create_parameters = 
        compress   = 1
        table_tyle = CSV
        file_name  = "test_table.zip"
    ;
}

source include/have_innodb.inc;
if($test_engine == "innodb"){
    let $innodb_compression_algorithm_orig = `SELECT @@Innodb_compression_algorithm`;
    
    eval SET GLOBAL Innodb_compression_algorithm = $test_library;
    let $table_create_parameters = page_compressed = 1;
    
    disable_query_log;
    CALL mtr.add_suppression("InnoDB: Background Page read failed to read or decrypt \\[page id: space=\\d+, page number=[123]\\]");
    eval CALL mtr.add_suppression("InnoDB: Table `test`.`$test_table_name` is corrupted. Please drop the table and recreate.");
    enable_query_log;
}

if($test_engine == "mroonga"){
    eval INSTALL SONAME "$HA_MROONGA_SO";

    let $column_flags = `SELECT UPPER('COMMENT \'flags "COLUMN_SCALAR|COMPRESS_$test_library"\'')`;
    let $table_create_parameters = charset = utf8;
}

if($test_engine == "rocksdb"){
    if($test_library == "bzip2"){
        let $library_flag = kBZip2Compression;
    }
    if($test_library == "lz4"){
        let $library_flag = kLZ4Compression;
    }
    if($test_library == "lz4hc"){
        let $library_flag = kLZ4HCCompression;
    }
    if($test_library == "snappy"){
        let $library_flag = kSnappyCompression;
    }
    if($test_library == "zstd"){
        let $library_flag = kZSTDNotFinalCompression;
    }

    let $restart_parameters = $restart_parameters --plugin-load=ha_rocksdb --rocksdb_default_cf_options=compression_per_level=$library_flag;

    exit;
}

eval CREATE TABLE $test_table_name (
        a int,
        b text $column_flags
    )
    engine = $test_engine
    $table_create_parameters
;

eval INSERT INTO $test_table_name (a, b) VALUES (0, "abc");
eval INSERT INTO $test_table_name (a, b) VALUES (1, "def");
eval INSERT INTO $test_table_name (a, b) VALUES (2, "ghi");

if($test_engine == "mroonga"){
    eval INSERT INTO $test_table_name (a, b) VALUES (3, "
        This is an extra long string to exceed Mroonga's compression threshold of 256 bytes (= 256 characters).
        Mroonga doesn't compress strings that are below the threshold, which
        means if the server is started without a compression library then for records:
            |         | < threshold | >= threshold      |
            |---------|-------------|-------------------|
            | Reading | Works fine  | Records are blank |
            | Writing | Works fine  | Throws an error   |
        Note that creating a compressed table will work even if the library is not loaded.
    ");
}

eval SELECT * FROM $test_table_name;


--echo # Start server with no libraries

let $restart_parameters = --use_compression=;
if($test_engine == "rocksdb"){
    let $restart_parameters = $restart_parameters --plugin-load=ha_rocksdb;
}

source include/restart_mysqld.inc;

if($test_engine == "innodb"){
    --echo # Table should not exist, as server does not know how to read compressed data
    error ER_NO_SUCH_TABLE_IN_ENGINE;
    eval SELECT * FROM $test_table_name;
}
if($test_engine == "mroonga"){
    --echo # Inserting a record below the threshold should work
    eval INSERT INTO $test_table_name (a, b) VALUES (4, "jkl");

    --echo # But inserting a record above the threshold should fail
    error ER_ERROR_ON_WRITE;
    eval INSERT INTO $test_table_name (a, b) VALUES (5, "
        0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ
        0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ
        0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ
        0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ
    ");

    --echo # Records below the threshold (abc, def, ghi) should appear, but records above the threshold should not
    eval SELECT * FROM $test_table_name;
}

eval DROP TABLE $test_table_name;



# reset system
disable_query_log;
if($test_engine == "innodb"){
    eval SET GLOBAL Innodb_compression_algorithm = $innodb_compression_algorithm_orig;
}
enable_query_log;

disable_warnings;
eval UNINSTALL SONAME IF EXISTS "$HA_CONNECT_SO";
eval UNINSTALL SONAME IF EXISTS "$HA_MROONGA_SO";
eval UNINSTALL SONAME IF EXISTS "$HA_ROCKSDB_SO";
enable_warnings;

let $restart_parameters =;
source include/restart_mysqld.inc;
