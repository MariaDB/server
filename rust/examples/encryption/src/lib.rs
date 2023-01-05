//! Basic encryption plugin using:
//!
//! - SHA256 as the hasher

#![allow(unused)]

use rand::Rng;
use sha2::{Digest, Sha256 as Hasher};
use std::sync::Mutex;
use std::time::{Duration, Instant};

use aes_gcm::{
    aead::{Aead, KeyInit, OsRng},
    Aes256Gcm,
    Nonce, // Or `Aes128Gcm`
};

use mariadb_server::plugin::encryption::{Encryption, Flags, KeyError, KeyManager, EncryptionError};
use mariadb_server::plugin::{Init, InitError, License, Maturity, PluginType};
// use mariadb_server::plugin::Init;
// use mariadb_server::plugin::prelude::*;

// plugin_encryption!{
//     type: RustEncryption,
//     init: RustEncryptionInit, // optional
//     name: "example_key_management",
//     author: "MariaDB Team",
//     description: "Example key management plugin using AES",
//     license: GPL,
//     stability: EXPERIMENTAL
// }

/// Range of key rotations, as seconds
const KEY_ROTATION_MIN: f32 = 45.0;
const KEY_ROTATION_MAX: f32 = 90.0;
const KEY_ROTATION_INTERVAL: f32 = KEY_ROTATION_MAX - KEY_ROTATION_MIN;
const SHA256_SIZE: usize = 32;
// const KEY_ROTATION_INTERVAL: Duration =
//     KEY_ROTATION_MAX - KEY_ROTATION_MIN;

/// Our global key version state
static KEY_VERSIONS: Mutex<Option<KeyVersions>> = Mutex::new(None);

/// Contain the state of our keys. We use `Instant` (the monotonically)
/// increasing clock) instead of `SystemTime` (which may occasionally go
/// backwards)
#[derive(Debug)]
struct KeyVersions {
    /// Initialization time of the struct, reference point for key version
    start: Instant,
    /// Most recent key update time
    current: Instant,
    /// Next time for a key update
    next: Instant,
}

impl KeyVersions {
    /// Initialize with a new value. Returns the struct
    fn new_now() -> Self {
        let now = Instant::now();
        let mut ret = Self {
            start: now,
            current: now,
            next: now,
        };
        ret.update_next();
        ret
    }

    fn update_next(&mut self) {
        let mult = rand::thread_rng().gen_range(0.0..1.0);
        let add_duration = KEY_ROTATION_MIN + mult * KEY_ROTATION_INTERVAL;
        self.next += Duration::from_secs_f32(add_duration);
    }

    /// Update the internal duration if needed, and return the elapsed time
    fn update_returning_version(&mut self) -> u64 {
        let now = Instant::now();
        if now > self.next {
            self.current = now;
            self.update_next();
        }
        (self.next - self.start).as_secs()
    }
}

struct RustEncryption;

impl Init for RustEncryption {
    /// Initialize function:
    fn init() -> Result<(), InitError> {
        eprintln!("init called for RustEncryption");
        let mut guard = KEY_VERSIONS.lock().unwrap();
        *guard = Some(KeyVersions::new_now());
        Ok(())
    }

    fn deinit() -> Result<(), InitError> {
        eprintln!("deinit called for RustEncryption");
        Ok(())
    }
}

impl KeyManager for RustEncryption {
    fn get_latest_key_version(_key_id: u32) -> Result<u32, KeyError> {
        dbg!(_key_id);
        let mut guard = KEY_VERSIONS.lock().unwrap();
        let mut vers = guard.as_mut().unwrap();
        Ok(vers.update_returning_version() as u32)
    }

    /// Given a key ID and a version, create its hash
    fn get_key(key_id: u32, key_version: u32, dst: &mut [u8]) -> Result<(), KeyError> {
        dbg!(key_id, key_version, dst.len());

        let output_size = Hasher::output_size();
        if dst.len() < output_size {
            return Err(KeyError::BufferTooSmall);
        }
        let mut hasher = Hasher::new();
        hasher.update(key_id.to_ne_bytes());
        hasher.update(key_version.to_ne_bytes());
        dst[..output_size].copy_from_slice(&hasher.finalize());
        Ok(())
    }

    fn key_length(key_id: u32, key_version: u32) -> Result<usize, KeyError> {
        dbg!(key_id, key_version);
        // All keys have the same length
        Ok(Hasher::output_size())
    }
}

impl Encryption for RustEncryption {
    fn init(
        key_id: u32,
        key_version: u32,
        key: &[u8],
        iv: &[u8],
        flags: Flags,
    ) -> Result<Self, EncryptionError> {
        eprintln!("encryption init");
        dbg!(&key_id, &key_version);
        eprintln!("key: {:x?}", &key);
        eprintln!("iv: {:x?}", &iv);
        dbg!(flags);
        Ok(Self)
    }

    fn update(&mut self, src: &[u8], dst: &mut [u8]) -> Result<(), EncryptionError> {
        eprintln!("encryption update");
        dbg!(src.len(), dst.len());
        dst[..src.len()].copy_from_slice(src);
        Ok(())
    }

    fn finish(&mut self, dst: &mut [u8]) -> Result<(), EncryptionError> {
        eprintln!("encryption finish");
        dbg!(dst.len());
        Ok(())
    }

    fn encrypted_length(key_id: u32, key_version: u32, src_len: usize) -> usize {
        eprintln!("encryption length");
        dbg!(key_id, key_version, src_len);
        src_len
    }
}

// C plugins manually create this, but we can automate
static _ENCRYPTION_ST: ::mariadb_server::plugin::UnsafeSyncCell<
    ::mariadb_server::bindings::st_mariadb_encryption,
> = unsafe {
    ::mariadb_server::plugin::UnsafeSyncCell::new(mariadb_server::bindings::st_mariadb_encryption {
        interface_version: mariadb_server::bindings::MariaDB_ENCRYPTION_INTERFACE_VERSION as i32,
        get_latest_key_version: Some(
            mariadb_server::plugin::encryption_wrapper::wrap_get_latest_key_version::<RustEncryption>,
        ),
        get_key: Some(mariadb_server::plugin::encryption_wrapper::wrap_get_key::<RustEncryption>),
        crypt_ctx_size: Some(
            mariadb_server::plugin::encryption_wrapper::wrap_crypt_ctx_size::<RustEncryption>,
        ),
        crypt_ctx_init: Some(
            mariadb_server::plugin::encryption_wrapper::wrap_crypt_ctx_init::<RustEncryption>,
        ),
        crypt_ctx_update: Some(
            mariadb_server::plugin::encryption_wrapper::wrap_crypt_ctx_update::<RustEncryption>,
        ),
        crypt_ctx_finish: Some(
            mariadb_server::plugin::encryption_wrapper::wrap_crypt_ctx_finish::<RustEncryption>,
        ),
        encrypted_length: Some(
            mariadb_server::plugin::encryption_wrapper::wrap_encrypted_length::<RustEncryption>,
        ),
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
        name: mariadb_server::cstr::cstr!("encryption_example").as_ptr(),
        author: mariadb_server::cstr::cstr!("Trevor Gross").as_ptr(),
        descr: mariadb_server::cstr::cstr!("Example key management / encryption plugin").as_ptr(),
        license: License::Gpl as i32,
        init: Some(mariadb_server::plugin::wrapper::wrap_init::<RustEncryption>),
        deinit: Some(mariadb_server::plugin::wrapper::wrap_deinit::<RustEncryption>),
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
