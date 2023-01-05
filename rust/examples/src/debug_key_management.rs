//! Debug key management
//!
//! Use to debug the encryption code with a fixed key that changes only on user
//! request. The only valid key ID is 1.
//!
//! EXAMPLE ONLY: DO NOT USE IN PRODUCTION!

#![allow(unused)]

use mariadb_server::plugin::encryption::{Encryption, Flags, KeyError, KeyManager};
use mariadb_server::plugin::PluginVarInfo;
use mariadb_server::plugin::SysVarAtomic;
use mariadb_server::sysvar_atomic;
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
