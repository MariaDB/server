//! Debug key management
//!
//! Use to debug the encryption code with a fixed key that changes only on user
//! request. The only valid key ID is 1.
//!
//! EXAMPLE ONLY: DO NOT USE IN PRODUCTION!

#![allow(unused)]

use mariadb_server::plugin::encryption::{Encryption, Flags, KeyError, KeyManager};
use mariadb_server::plugin::{Init, License, Maturity, SysVarAtomic};
use mariadb_server::plugin::{InitError, PluginType, PluginVarInfo};
use mariadb_server::sysvar_atomic;
use std::cell::UnsafeCell;
use std::ffi::c_void;
use std::sync::atomic::AtomicU32;
use std::sync::atomic::Ordering;

const KEY_LENGTH: usize = 4;

static KEY_VERSION: AtomicU32 = AtomicU32::new(0);

static KEY_VERSION_SYSVAR: SysVarAtomic<u32> = sysvar_atomic! {
    ty: u32,
    name: "version",
    var: KEY_VERSION,
    comment: "Latest key version",
    flags: [PluginVarInfo::ReqCmdArg],
    default: 1,
};

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
        dst.copy_from_slice(key_buf.as_slice());
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

// bindings::st_maria_plugin {
//     type_: PluginType::Encryption,
//     info: *mut c_void,
//     name: cstr::cstr!($name).as_ptr(),
//     author: cstr::cstr!($author).as_ptr(),
//     descr: cstr::cstr!($description).as_ptr(),
//     license: c_int,
//     init: Option<unsafe extern "C" fn(arg1: *mut c_void) -> c_int>,
//     deinit: Option<unsafe extern "C" fn(arg1: *mut c_void) -> c_int>,
//     version: vers as c_uint,
//     status_vars: *mut st_mysql_show_var,
//     system_vars: *mut *mut st_mysql_sys_var,
//     version_info: plugin!(
//         @def
//         cstr::cstr!($vers).as_ptr(),
//         $(cstr::cstr!($comment).as_ptr())?
//     ),,
//     maturity: maturity as c_uint,
// }

// #![crate_type = "cdylib"]

// builtin_debug_key_plugin_interface_version
// builtin_debug_key_sizeof_struct_st_plugin
// builtin_debug_key_plugin

#[repr(transparent)]
struct UcWrap<T>(UnsafeCell<T>);

impl<T> UcWrap<T> {
    const fn as_ptr(&self) -> *const T {
        self.0.get()
    }
}

unsafe impl<T> Send for UcWrap<T> {}
unsafe impl<T> Sync for UcWrap<T> {}

// PROC: should mangle names with type name

static _INTERNAL_SYSVARS: UcWrap<[*mut mariadb_server::bindings::st_mysql_sys_var; 2]> =
    UcWrap(UnsafeCell::new([
        KEY_VERSION_SYSVAR.as_ptr().cast_mut(),
        ::std::ptr::null_mut(),
    ]));

static _ENCRYPTION_ST: UcWrap<mariadb_server::bindings::st_mariadb_encryption> = UcWrap(
    UnsafeCell::new(mariadb_server::bindings::st_mariadb_encryption {
        interface_version: mariadb_server::bindings::MariaDB_ENCRYPTION_INTERFACE_VERSION as i32,
        get_latest_key_version: Some(
            mariadb_server::plugin::encryption_wrapper::wrap_get_latest_key_version::<DebugKeyMgmt>,
        ),
        get_key: Some(mariadb_server::plugin::encryption_wrapper::wrap_get_key::<DebugKeyMgmt>),
        crypt_ctx_size: None,
        crypt_ctx_init: None,
        crypt_ctx_update: None,
        crypt_ctx_finish: None,
        encrypted_length: None,
    }),
);

#[no_mangle]
static _maria_plugin_interface_version_: ::std::ffi::c_int =
    mariadb_server::bindings::MARIA_PLUGIN_INTERFACE_VERSION as ::std::ffi::c_int;

#[no_mangle]
static _maria_sizeof_struct_st_plugin_: ::std::ffi::c_int =
    ::std::mem::size_of::<mariadb_server::bindings::st_maria_plugin>() as ::std::ffi::c_int;

#[no_mangle]
static mut _maria_plugin_declarations_: [mariadb_server::bindings::st_maria_plugin; 2] = [
    mariadb_server::bindings::st_maria_plugin {
        type_: PluginType::MariaEncryption as i32,
        info: _ENCRYPTION_ST.as_ptr().cast_mut().cast(),
        name: mariadb_server::cstr::cstr!("debug_key_management").as_ptr(),
        author: mariadb_server::cstr::cstr!("Trevor Gross").as_ptr(),
        descr: mariadb_server::cstr::cstr!("Debug key management plugin").as_ptr(),
        license: License::Gpl as i32,
        init: Some(mariadb_server::plugin::wrapper::wrap_init::<DebugKeyMgmt>),
        deinit: Some(mariadb_server::plugin::wrapper::wrap_deinit::<DebugKeyMgmt>),
        version: 0x0010,
        status_vars: ::std::ptr::null_mut(),
        system_vars: _INTERNAL_SYSVARS.0.get().cast(),
        version_info: mariadb_server::cstr::cstr!("0.1").as_ptr(),
        maturity: Maturity::Experimental as u32,
    },
    // End with a null object
    ::mariadb_server::plugin::new_null_st_maria_plugin(),
];
