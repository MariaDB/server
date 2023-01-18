//! Basic encryption plugin using:
//!
//! - SHA256 as the hasher

#![allow(unused)]

use std::sync::Mutex;
use std::time::{Duration, Instant};

use aes_gcm::{
    aead::{Aead, KeyInit, OsRng},
    Aes256Gcm,
    Nonce, // Or `Aes128Gcm`
};
use mariadb::plugin::encryption::{Encryption, EncryptionError, Flags, KeyError, KeyManager};
use mariadb::plugin::prelude::*;
use mariadb::plugin::wrapper::WrapInit;
use rand::Rng;
use sha2::{Digest, Sha256 as Hasher};

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

register_plugin! {
    RustEncryption,
    ptype: PluginType::MariaEncryption,
    name: "encryption_example",
    author: "Trevor Gross",
    description: "Example key management / encryption plugin",
    license: License::Gpl,
    maturity: Maturity::Experimental,
    version: "0.1",
    init: RustEncryption, // optional
    encryption: true,
}

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
