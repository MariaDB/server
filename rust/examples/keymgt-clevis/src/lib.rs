//! EXAMPLE ONLY: DO NOT USE IN PRODUCTION!

#![allow(unused)]

use std::cell::UnsafeCell;
use std::ffi::c_void;
use std::sync::atomic::{AtomicU32, Ordering};

use mariadb::plugin::encryption::{Encryption, Flags, KeyError, KeyManager};
use mariadb::plugin::prelude::*;
use mariadb::plugin::{
    register_plugin, Init, InitError, License, Maturity, PluginType, PluginVarInfo, SysVarAtomic,
};
use mariadb::service_sql::MySqlConn;
use mariadb::{debug, info, sysvar_atomic};

const KEY_TABLE: &str = "mysql.clevis_keys";
const SERVER_TABLE: &str = "mysql.clevis_servers";

struct KeyMgtClevis;

impl Init for KeyMgtClevis {
    /// Create needed tables
    fn init() -> Result<(), InitError> {
        debug!("init for KeyMgtClevis");

        let conn = MySqlConn::connect_local().map_err(|_| InitError)?;
        conn.execute(&format!(
            "CREATE TABLE IF NOT EXISTS {KEY_TABLE} (
            key_id INT UNSIGNED NOT NULL,
            key_version INT UNSIGNED NOT NULL,
            key_server VARBINARY(64) NOT NULL,
            key VARBINARY((16) NOT NULL,
            PRIMARY KEY (key_id, key_version)
            ) ENGINE=InnoDB"
        ))
        .map_err(|_| InitError)?;
        conn.execute(&format!(
            "CREATE TABLE IF NOT EXISTS {SERVER_TABLE} (
            server VARBINARY(64) NOT NULL PRIMARY KEY,
            verify VARBINARY(2048)
            enc VARBINARY(2048)
            ) ENGINE=InnoDB"
        ))
        .map_err(|_| InitError)?;

        debug!("finished init for KeyMgtClevis");
        Ok(())
    }

    fn deinit() -> Result<(), InitError> {
        debug!("deinit for KeyMgtClevis");
        Ok(())
    }
}

impl KeyManager for KeyMgtClevis {
    fn get_latest_key_version(key_id: u32) -> Result<u32, KeyError> {
        let conn = MySqlConn::connect_local().map_err(|_| KeyError::Other)?;
        conn.query(&format!(
            "SELECT key_version FROM {KEY_TABLE} WHERE key_id = {key_id}"
        ))
        .map_err(|_| KeyError::Other)?;

        todo!()
    }

    fn get_key(key_id: u32, key_version: u32, dst: &mut [u8]) -> Result<(), KeyError> {
        todo!()
    }

    fn key_length(key_id: u32, key_version: u32) -> Result<usize, KeyError> {
        todo!()
    }
}

register_plugin! {
    KeyMgtClevis,
    ptype: PluginType::MariaEncryption,
    name: "clevis_key_management",
    author: "Trevor Gross",
    description: "Clevis key management plugin",
    license: License::Gpl,
    maturity: Maturity::Experimental,
    version: "0.1",
    init: KeyMgtClevis, // optional
    encryption: false,
}
