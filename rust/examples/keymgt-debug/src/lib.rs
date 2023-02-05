//! Debug key management
//!
//! Use to debug the encryption code with a fixed key that changes only on user
//! request. The only valid key ID is 1.
//!
//! EXAMPLE ONLY: DO NOT USE IN PRODUCTION!

#![allow(unused)]

use std::cell::UnsafeCell;
use std::ffi::c_void;
use std::sync::atomic::{AtomicU32, Ordering};

use mariadb::plugin::encryption::{Encryption, Flags, KeyError, KeyManager};
use mariadb::plugin::prelude::*;
use mariadb::plugin::{
    register_plugin, Init, InitError, License, Maturity, PluginType, PluginVarInfo,
};
use mariadb::sysvar_atomic;

const KEY_LENGTH: usize = 4;

static KEY_VERSION: AtomicU32 = AtomicU32::new(1);

// static KEY_VERSION_SYSVAR: SysVarAtomic<u32> = sysvar_atomic! {
//     ty: u32,
//     name: "version",
//     var: KEY_VERSION,
//     comment: "Latest key version",
//     flags: [PluginVarInfo::ReqCmdArg],
//     default: 1,
// };

struct DebugKeyMgmt;

impl Init for DebugKeyMgmt {
    fn init() -> Result<(), InitError> {
        eprintln!("init for DebugKeyMgmt");
        Ok(())
    }

    fn deinit() -> Result<(), InitError> {
        eprintln!("deinit for DebugKeyMgmt");
        Ok(())
    }
}

impl KeyManager for DebugKeyMgmt {
    fn get_latest_key_version(key_id: u32) -> Result<u32, KeyError> {
        if key_id != 1 {
            Err(KeyError::VersionInvalid)
        } else {
            Ok(KEY_VERSION.load(Ordering::Relaxed))
        }
    }

    fn get_key(key_id: u32, key_version: u32, dst: &mut [u8]) -> Result<(), KeyError> {
        if key_id != 1 {
            return Err(KeyError::VersionInvalid);
        }

        // Convert our integer to a native endian byte array
        let key_buf = KEY_VERSION.load(Ordering::Relaxed).to_ne_bytes();

        if dst.len() < key_buf.len() {
            return Err(KeyError::BufferTooSmall);
        }

        // Copy our slice to the buffer, return the copied length
        dst[..key_buf.len()].copy_from_slice(key_buf.as_slice());
        Ok(())
    }

    fn key_length(key_id: u32, key_version: u32) -> Result<usize, KeyError> {
        // Return the length of our u32 in bytes
        // Just verify our types don't change
        debug_assert_eq!(
            KEY_LENGTH,
            KEY_VERSION.load(Ordering::Relaxed).to_ne_bytes().len()
        );
        Ok(KEY_LENGTH)
    }
}

register_plugin! {
    DebugKeyMgmt,
    ptype: PluginType::MariaEncryption,
    name: "debug_key_management",
    author: "Trevor Gross",
    description: "Debug key management plugin",
    license: License::Gpl,
    maturity: Maturity::Experimental,
    version: "0.1",
    init: DebugKeyMgmt, // optional
    encryption: false,
}

// PROC: should mangle names with type name

// static _INTERNAL_SYSVARS: UcWrap<[*mut mariadb::bindings::st_mysql_sys_var; 2]> =
//     UcWrap(UnsafeCell::new([
//         KEY_VERSION_SYSVAR.as_ptr().cast_mut(),
//         ::std::ptr::null_mut(),
//     ]));
