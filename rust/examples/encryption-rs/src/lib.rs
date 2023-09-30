//! Basic encryption plugin using:
//!
//! - SHA256 as the hasher to create a key
//! - Randomized key update times
//! - No encryption, just copies from src to dst
//!
//! This is noisy, prints a ton so it is easy to see what is going on

#![allow(unused)]

use std::sync::Mutex;
use std::time::{Duration, Instant};

use aes_gcm::{
    aead::{Aead, KeyInit, OsRng},
    Aes256Gcm,
    Nonce, // Or `Aes128Gcm`
};
use mariadb::log::info;
use mariadb::plugin::encryption::{Encryption, EncryptionError, Flags, KeyError, KeyManager};
use mariadb::plugin::*;
use rand::Rng;
use sha2::{Digest, Sha256};

/// Range of key rotations, as seconds
const KEY_ROTATION_MIN: f32 = 45.0;
const KEY_ROTATION_MAX: f32 = 90.0;
const KEY_ROTATION_INTERVAL: f32 = KEY_ROTATION_MAX - KEY_ROTATION_MIN;
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

    /// Create a random time for the next key update
    fn update_next(&mut self) {
        let mult = rand::thread_rng().gen_range(0.0..1.0);
        let add_duration = KEY_ROTATION_MIN + mult * KEY_ROTATION_INTERVAL;
        self.next += Duration::from_secs_f32(add_duration);
    }

    /// Check if we need to update the ksy, i.e. if our elapsed time has passed,
    /// and return the current key
    fn update_returning_version(&mut self) -> u32 {
        let now = Instant::now();
        if now > self.next {
            self.current = now;
            self.update_next();
            info!("updating to {:?}", self);
        }
        (self.next - self.start).as_secs().try_into().unwrap()
    }
}

#[derive(Debug)]
struct EncryptionExampleRs(usize);

impl Init for EncryptionExampleRs {
    /// Initialize function:
    fn init() -> Result<(), InitError> {
        let mut guard = KEY_VERSIONS.lock().unwrap();
        *guard = Some(KeyVersions::new_now());
        Ok(())
    }

    fn deinit() -> Result<(), InitError> {
        Ok(())
    }
}

impl KeyManager for EncryptionExampleRs {
    fn get_latest_key_version(key_id: u32) -> Result<u32, KeyError> {
        info!("get_latest_key_version id {key_id}");
        let mut guard = KEY_VERSIONS.lock().unwrap();
        let mut vers = guard.as_mut().unwrap();
        Ok(vers.update_returning_version())
    }

    /// Given a key ID and a version, hash them together to create the key
    fn get_key(key_id: u32, key_version: u32, dst: &mut [u8]) -> Result<(), KeyError> {
        info!(
            "get_key id {key_id} version {key_version} dst len {}",
            dst.len()
        );
        let output_size = Sha256::output_size();
        if dst.len() < output_size {
            return Err(KeyError::BufferTooSmall);
        }
        let mut hasher = Sha256::new();
        dst[..output_size].copy_from_slice(&hasher.finalize());
        Ok(())
    }

    fn key_length(key_id: u32, key_version: u32) -> Result<usize, KeyError> {
        info!("KEY_LENGTH id {key_id} version {key_version}");
        // All keys have the same length
        Ok(Sha256::output_size())
    }
}

/// Our encryption does nothing! Just copies
impl Encryption for EncryptionExampleRs {
    fn init(
        key_id: u32,
        key_version: u32,
        key: &[u8],
        iv: &[u8],
        flags: Flags,
    ) -> Result<Self, EncryptionError> {
        info!("encryption_init, id {key_id}, version {key_version}");
        info!("key: {:x?}", &key);
        info!("iv: {:x?}", &iv);
        Ok(Self(0))
    }

    fn update(&mut self, src: &[u8], dst: &mut [u8]) -> Result<usize, EncryptionError> {
        info!(
            "encryption_update, src_len {}, dst_len {}",
            src.len(),
            dst.len()
        );
        dst[..src.len()].copy_from_slice(src);
        self.0 += src.len();
        Ok(src.len())
    }

    fn finish(&mut self, dst: &mut [u8]) -> Result<usize, EncryptionError> {
        info!("encryption_finish, dst_len {}", dst.len());
        Ok(0)
    }

    /// Always same input and output length
    fn encrypted_length(key_id: u32, key_version: u32, src_len: usize) -> usize {
        info!("encryption_length, id {key_id}, version {key_version}, src_len {src_len}");
        src_len
    }
}

register_plugin! {
    EncryptionExampleRs,
    ptype: PluginType::MariaEncryption,
    name: "encryption_example",
    author: "Trevor Gross",
    description: "Example key management / encryption plugin",
    license: License::Gpl,
    maturity: Maturity::Experimental,
    version: "0.1",
    init: EncryptionExampleRs, // optional
    encryption: true,
}
