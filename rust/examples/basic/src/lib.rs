//! Debug key management
//!
//! Use to debug the encryption code with a fixed key that changes only on user
//! request. The only valid key ID is 1.
//!
//! EXAMPLE ONLY: DO NOT USE IN PRODUCTION!

#![allow(unused)]

use mariadb_server::plugin::encryption::{Encryption, Flags, KeyError, KeyManager};
use mariadb_server::plugin::{new_null_st_maria_plugin, PluginType, PluginVarInfo, UnsafeSyncCell};
use mariadb_server::plugin::{License, Maturity, SysVarAtomic, InitError, Init};
use mariadb_server::sysvar_atomic;
use std::cell::UnsafeCell;
use std::ffi::c_void;
use std::sync::atomic::Ordering;
use std::sync::atomic::{AtomicU32, AtomicUsize};

struct BasicKeyMgt;

static COUNTER: AtomicUsize = AtomicUsize::new(30);

impl Init for BasicKeyMgt {
    fn init() -> Result<(), InitError> {
        eprintln!("init for BasicKeyMgt");
        Ok(())
    }

    fn deinit() -> Result<(), InitError> {
        eprintln!("deinit for BasicKeyMgt");
        Ok(())
    }
}

impl KeyManager for BasicKeyMgt {
    fn get_latest_key_version(key_id: u32) -> Result<u32, KeyError> {
        static KCOUNT: AtomicU32 = AtomicU32::new(1);
        eprintln!("get key version with {key_id}");
        Ok(KCOUNT.fetch_add(1, Ordering::Relaxed))
    }

    fn get_key(key_id: u32, key_version: u32, dst: &mut [u8]) -> Result<(), KeyError> {
        let s = format!("get_key: {key_id}:{key_version}");
        eprintln!("{s}, {}", dst.len());

        if dst.len() < dbg!(COUNTER.fetch_add(1, Ordering::Relaxed)) {
            return Err(KeyError::BufferTooSmall);
        }

        // Copy our slice to the buffer, return the copied length
        dst[..s.len()].copy_from_slice(s.as_str().as_bytes());
        Ok(())
    }

    fn key_length(key_id: u32, key_version: u32) -> Result<usize, KeyError> {
        eprintln!("get key length with {key_id}:{key_version}");
        Ok(COUNTER.load(Ordering::Relaxed))
    }
}

// PROC: should mangle names with type name

// C plugins manually create this, but we can automate
static _ENCRYPTION_ST: ::mariadb_server::plugin::UnsafeSyncCell<
    ::mariadb_server::bindings::st_mariadb_encryption,
> = unsafe {
    ::mariadb_server::plugin::UnsafeSyncCell::new(mariadb_server::bindings::st_mariadb_encryption {
        interface_version: mariadb_server::bindings::MariaDB_ENCRYPTION_INTERFACE_VERSION as i32,
        get_latest_key_version: Some(
            mariadb_server::plugin::encryption_wrapper::wrap_get_latest_key_version::<BasicKeyMgt>,
        ),
        get_key: Some(mariadb_server::plugin::encryption_wrapper::wrap_get_key::<BasicKeyMgt>),
        crypt_ctx_size: None,
        crypt_ctx_init: None,
        crypt_ctx_update: None,
        crypt_ctx_finish: None,
        encrypted_length: None,
    })
};

// If we compile dynamically, use these names. Otherwise, we need to use
// `buildin_maria_NAME_...`
#[no_mangle]
static _maria_plugin_interface_version_: ::std::ffi::c_int =
    ::mariadb_server::bindings::MARIA_PLUGIN_INTERFACE_VERSION as ::std::ffi::c_int;

#[no_mangle]
static _maria_sizeof_struct_st_plugin_: ::std::ffi::c_int =
    ::std::mem::size_of::<mariadb_server::bindings::st_maria_plugin>() as ::std::ffi::c_int;

#[no_mangle]
static mut _maria_plugin_declarations_: [mariadb_server::bindings::st_maria_plugin; 2] = [
    ::mariadb_server::bindings::st_maria_plugin {
        type_: PluginType::MariaEncryption as i32,
        info: _ENCRYPTION_ST.as_ptr().cast_mut().cast(),
        name: mariadb_server::cstr::cstr!("basic_key_management").as_ptr(),
        author: mariadb_server::cstr::cstr!("Trevor Gross").as_ptr(),
        descr: mariadb_server::cstr::cstr!("Basic key management plugin").as_ptr(),
        license: License::Gpl as i32,
        init: Some(mariadb_server::plugin::wrapper::wrap_init::<BasicKeyMgt>),
        deinit: Some(mariadb_server::plugin::wrapper::wrap_deinit::<BasicKeyMgt>),
        version: 0x0010,
        status_vars: ::std::ptr::null_mut(),
        system_vars: ::std::ptr::null_mut(),
        version_info: mariadb_server::cstr::cstr!("0.1").as_ptr(),
        maturity: Maturity::Experimental as u32,
    },
    // End with a null object
    ::mariadb_server::plugin::new_null_st_maria_plugin(),
];

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn print_statics() {
        unsafe { dbg!(&*(_ENCRYPTION_ST.as_ptr())) };
        dbg!(&_maria_plugin_interface_version_);
        dbg!(&_maria_sizeof_struct_st_plugin_);
        unsafe { dbg!(&_maria_plugin_declarations_) };
    }
}
