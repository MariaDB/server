#![allow(unused)]

use std::time::{Instant, Duration};
use std::sync::Mutex;
use rand::{Rng};

use mariadb_server::plugin::encryption::{KeyError, Encryption};
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
// const KEY_ROTATION_INTERVAL: Duration =
//     KEY_ROTATION_MAX - KEY_ROTATION_MIN;

/// Our global key version state
static KEY_VERSIONS: Mutex<Option<KeyVersions>> = Mutex::new(None);

/// Contain the state of our keys. We use `Instant` (the monotonically)
/// increasing clock) instead of `SystemTime` (which may occasionally go
/// backwards)
struct KeyVersions {
    /// Initialization time of the struct, reference point for key version
    start: Instant,
    /// Most recent key update time
    current: Instant,
    /// Next time for a key update
    next: Instant
}

impl KeyVersions {
    /// Initialize with a new value. Returns the struct 
    fn new_now() -> (Self, u64) {
        let now = Instant::now();
        let mut ret = Self {
            start: now,
            current: now,
            next: now
        };
        ret.update_next();
        let duration = (ret.next - ret.start).as_secs();
        (ret, duration)
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

// Uninitialized 


// struct RustEncryptionInit;

// impl plugin::Init for RustEncryptionInit {
//     fn init() -> Self {
//         let mut versions = KEY_VERSIONS.lock();
//         *versions = Some(KeyVersions::random_from_now());
//         Self
//     }
// }


struct RustEncryption;

impl Encryption for RustEncryption {
    fn get_latest_key_version(_key_id: u32) -> Result<u32, KeyError> {
        let mut guard = KEY_VERSIONS.lock().unwrap();
        if let Some(ref mut vers) = *guard {
            Ok(vers.update_returning_version() as u32)
        } else {
            let (kv, ret) = KeyVersions::new_now();
            *guard = Some(kv);
            Ok(ret as u32)
        }
    }
    
    /// Given a key ID and a 
    fn get_key(key_id: u32, key_version: u32, key: &mut [u8]) -> Result<usize, KeyError>  {
        
        todo!()
    }
    
    fn get_key_length(key_id: u32, key_version: u32) -> Result<usize, KeyError> {
        todo!()
    }

    /// Initialize
    fn init(key_id: u32, key_version: u32, key: &[u8], iv: &[u8], flags: u32) -> Self {
        todo!()

    }

    /// Initialize
    fn update(ctx: &mut Self, src: &[u8], dst: &mut [u8]) -> usize {
        todo!()
    }

    fn finish(ctx: &mut Self, dst: &mut [u8]) -> usize{
        todo!()
    }
    
    fn encrypted_length(key_id: u32, key_version: u32, src_len: usize) {
        todo!()
    }
}
