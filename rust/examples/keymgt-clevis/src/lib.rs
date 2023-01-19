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
use mariadb::sysvar_atomic;

struct KeyMgtClevis;

impl Init for KeyMgtClevis {
    fn init() -> Result<(), InitError> {
        eprintln!("init for KeyMgtClevis");
        Ok(())
    }

    fn deinit() -> Result<(), InitError> {
        eprintln!("deinit for KeyMgtClevis");
        Ok(())
    }
}

impl KeyManager for KeyMgtClevis {
    fn get_latest_key_version(key_id: u32) -> Result<u32, KeyError> {
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

// PROC: should mangle names with type name

// static _INTERNAL_SYSVARS: UcWrap<[*mut mariadb::bindings::st_mysql_sys_var; 2]> =
//     UcWrap(UnsafeCell::new([
//         KEY_VERSION_SYSVAR.as_ptr().cast_mut(),
//         ::std::ptr::null_mut(),
//     ]));
