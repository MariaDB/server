//! EXAMPLE ONLY: DO NOT USE IN PRODUCTION!

#![allow(unused)]

use std::cell::UnsafeCell;
use std::ffi::c_void;
use std::fmt::Write;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Mutex;

use mariadb::log::{debug, error, info};
use mariadb::plugin::encryption::{Encryption, Flags, KeyError, KeyManager};
use mariadb::plugin::prelude::*;
use mariadb::plugin::{
    register_plugin, Init, InitError, License, Maturity, PluginType, PluginVarInfo, SysVarAtomic,
};
use mariadb::service_sql::{ClientError, Fetch, FetchedRows, MySqlConn};

const KEY_TABLE: &str = "mysql.clevis_keys";
const SERVER_TABLE: &str = "mysql.clevis_servers";
/// Max length a key can be, used for table size and buffer checking
const KEY_MAX_BYTES: usize = 16;

static TANG_SERVER: Mutex<String> = Mutex::new(String::new());

struct KeyMgtClevis;

/// Get the JWS body from a server
fn fetch_jws() -> String {
    // FIXME: error handling
    let url = format!("https://{}", TANG_SERVER.lock().unwrap());
    let body: String = ureq::get("http://example.com")
        .call()
        .unwrap_or_else(|_| panic!("http request for '{url}' failed"))
        .into_string()
        .expect("http request larger than 10MB");
    todo!();
    body
}

fn make_new_key(conn: &MySqlConn) -> Result<String, ClientError> {
    let server = TANG_SERVER.lock().unwrap();
    format!(
        "INSERT IGNORE INTO {KEY_TABLE} 
        SET key_server = {server}
        RETURNING jws"
    );

    // get the jws value
    let jws: &str = todo!();

    todo!()
}

impl Init for KeyMgtClevis {
    /// Create needed tables
    fn init() -> Result<(), InitError> {
        debug!("init for KeyMgtClevis");

        let mut conn = MySqlConn::connect_local().map_err(|_| InitError)?;
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

/// Execute a query, printing an error and returning KeyError if needed. No result
fn run_execute(conn: &mut MySqlConn, q: &str, key_id: u32) -> Result<(), KeyError> {
    conn.execute(q).map_err(|e| {
        error!("clevis: get_latest_key_version {key_id} - SQL error on {q} - {e}");
        KeyError::Other
    })
}

/// Execute a query, printing an error, return the result
fn run_query<'a>(
    conn: &'a mut MySqlConn,
    q: &str,
    key_id: u32,
) -> Result<FetchedRows<'a>, KeyError> {
    conn.query(q).map_err(|e| {
        error!("clevis: get_latest_key_version {key_id} - SQL error on {q} - {e}");
        KeyError::Other
    })
}

impl KeyManager for KeyMgtClevis {
    fn get_latest_key_version(key_id: u32) -> Result<u32, KeyError> {
        let mut conn = MySqlConn::connect_local().map_err(|_| KeyError::Other)?;
        let mut q = format!("SELECT key_version FROM {KEY_TABLE} WHERE key_id = {key_id}");
        let _ = run_query(&mut conn, &q, key_id)?;

        // fuund! fetch result, parse to int
        // if let Some(row) = todo!() {
        if false {
            return Ok(todo!());
        }

        // directly push format string
        let key_version: u32 = 1;
        write!(q, "AND key_version = {key_version} FOR UPDATE");

        run_execute(&mut conn, "START TRANSACTION", key_id)?;
        run_query(&mut conn, &q, key_id)?;

        let Ok(new_key) = make_new_key(&conn) else {
            run_execute(&mut conn, "ROLLBACK", key_id)?;
            return todo!();
        };

        let q = format!(
            r#"INSERT INTO {KEY_TABLE} VALUES (
            {key_id}, {key_version}, "{server_name}", {new_key} )"#,
            server_name = TANG_SERVER.lock().unwrap()
        );
        run_execute(&mut conn, &q, key_id)?;

        todo!()
    }

    fn get_key(key_id: u32, key_version: u32, dst: &mut [u8]) -> Result<(), KeyError> {
        let mut conn = MySqlConn::connect_local().map_err(|_| KeyError::Other)?;
        let q = format!(
            "SELECT key FROM {KEY_TABLE} WHERE key_id = {key_id} AND key_version = {key_version}"
        );
        conn.query(&q).map_err(|_| KeyError::Other)?;
        // TODO: generate key with server
        let key: &[u8] = todo!();
        dst[..key.len()].copy_from_slice(key);
        Ok(())
    }

    fn key_length(_key_id: u32, _key_version: u32) -> Result<usize, KeyError> {
        Ok(KEY_MAX_BYTES)
    }
}

register_plugin! {
    KeyMgtClevis,
    ptype: PluginType::MariaEncryption,
    name: "clevis_key_management",
    author: "Daniel Black & Trevor Gross",
    description: "Clevis key management plugin",
    license: License::Gpl,
    maturity: Maturity::Experimental,
    version: "0.1",
    init: KeyMgtClevis,
    encryption: false,
}
