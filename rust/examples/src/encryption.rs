use std::time::{Instant, Duration};
use std::sync::Mutex;

use mariadb_server::plugin;
// use mariadb_server::plugin::prelude::*;

plugin_encryption!{
    type: RustEncryption,
    init: RustEncryptionInit, // optional
    name: "example_key_management",
    author: "MariaDB Team",
    description: "Example key management plugin using AES",
    license: GPL,
    stability: EXPERIMENTAL
}


const KEY_ROTATION_MIN: u64 = 45;
const KEY_ROTATION_MAX: u64 = 90;
const KEY_ROTATION_INTERVAL: Duration =
    Duration::from_secs(KEY_ROTATION_MAX - KEY_ROTATION_MIN);

struct KeyVersions {
    current: Instant,
    next: Instant
}

impl KeyVersions {
    fn random_from_now() -> Self {
        let now = Instant::now();
        let next = now + KEY_ROTATION_MIN + random;
        Self {
            current: now,
            next: next
        }
    }
}

// Uninitialized 
static KEY_VERSIONS: Mutex<Option<KeyVersions>> = Mutex::new(None);

struct RustEncryptionInit;



impl plugin::Init for RustEncryptionInit {
    fn init() -> Self {
        let mut versions = KEY_VERSIONS.lock();
        *versions = Some(KeyVersions::random_from_now());
        Self
    }
}


struct RustEncryption;

impl plugin::Encryption for RustEncryption {
    fn get_latest_key_version(key_id: u32) -> Result<u32, EncryptionKeyVersionInvalid> {

    }

    /// Given a key ID and a 
    fn get_key(key_id: u32, version: u32, key: &mut BufVec)  -> Result<(), EncryptionError>  {

    }

    /// Initialize
    fn init(key: &[u8], iv: &[u8], flags: u32, key_id: u32, ) -> Self {

    }

    /// Initialize
    fn update(&mut self, src: &[u8], dst: &mut BufVec);

    fn finish(&mut self, dst: DestBuf);

    fn size(key_id: u32, key_version: u32) -> u32;

    fn encrypted_length(len: u32);
}
