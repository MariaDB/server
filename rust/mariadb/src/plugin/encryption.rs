//! Requirements to implement an encryption plugin
//!
//! # Usage
//!
//! - Keep key storage context in globals. These need to be mutex-protected
//!
//! # Implementation
//!
//! `plugin_encryption.h` defines `st_mariadb_encryption`, with the following members:
//!
//! - `interface_version`: integer, set via macro
//! - `get_latest_key_version`: function, wrapped in `Encryption::get_latest_key_version`
//! - `get_key`: function, wrapped in `Encryption::get_key`
//! - `crypt_ctx_size`: function, wrapped in `Encryption::size`
//! - `crypt_ctx_init`: function, wrapped in `Encryption::init`
//! - `crypt_ctx_update`: function, wrapped in `Encryption::update`
//! - `crypt_ctx_finish`: function, wrapped in `Encryption::finish`
//! - `encrypted_length`: function, macro provides call to `std::mem::size_of`

use core::cell::UnsafeCell;
use mariadb_server_sys as bindings;

/// A type of error to be used by key functions
#[repr(u32)]
#[non_exhaustive]
pub enum KeyError {
    VersionInvalid,
    BufferTooSmall,
    Other,
}

/// Implement this trait on a struct that will serve as encryption context
///
///
#[allow(unused_variables)]
pub trait Encryption {
    /// The type of context data passed to various functions
    type Context: Send;

    /// Get the latest version of a key ID. Return `VersionInvalid` if not found.
    fn get_latest_key_version(key_id: u32) -> Result<u32, KeyError>;

    /// Return a key for a key version
    ///
    /// Given a key ID and a version, write the key to the `key` buffer. If the
    /// buffer is too small, return [`KeyError::BufferTooSmall`].
    fn get_key(key_id: u32, key_version: u32, key: &mut [u8]) -> Result<usize, KeyError>;

    /// Calculate the length of a key
    fn get_key_length(key_id: u32, key_version: u32) -> Result<usize, KeyError>;

    /// Initialize
    fn init(key_id: u32, key_version: u32, key: &[u8], iv: &[u8], flags: u32) -> Self::Context {
        unimplemented!()
    }

    /// Initialize
    fn update(ctx: &mut Self::Context, src: &[u8], dst: &mut [u8]) {
        unimplemented!()
    }

    fn finish(ctx: &mut Self::Context, dst: &mut [u8]) {
        unimplemented!()
    }

    fn encrypted_length(key_id: u32, key_version: u32, slen: usize) {
        unimplemented!()
    }
}

// get_latest_key_version: #type::get_latest_key_version
// crypt_ctx_size: std::mem::size_of<#type::Context>()
// crypt_ctx_init: #type:init()
