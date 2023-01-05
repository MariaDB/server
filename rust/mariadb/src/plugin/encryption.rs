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

// use core::cell::UnsafeCell;
use mariadb_server_sys as bindings;

/// A type of error to be used by key functions
#[non_exhaustive]
#[repr(u32)]
pub enum KeyError {
    // Values must be nonzero
    /// A key ID is invalid or not found. Maps to `ENCRYPTION_KEY_VERSION_INVALID` in C.
    VersionInvalid = bindings::ENCRYPTION_KEY_VERSION_INVALID,
    /// A key buffer is too small. Maps to `ENCRYPTION_KEY_BUFFER_TOO_SMALL` in C.
    BufferTooSmall = bindings::ENCRYPTION_KEY_BUFFER_TOO_SMALL,
    Other = 3,
}

/// Representation of the flags integer
#[derive(Clone, Copy, PartialEq)]
pub struct Flags(u32);

impl Flags {
    fn should_encrypt(&self) -> bool {
        (self.0 & bindings::ENCRYPTION_FLAG_ENCRYPT) != 0
    }

    fn should_decrypt(&self) -> bool {
        (self.0 & bindings::ENCRYPTION_FLAG_ENCRYPT) != 0
    }

    pub fn nopad(&self) -> bool {
        (self.0 & bindings::ENCRYPTION_FLAG_NOPAD) != 0
    }
}

/// Implement this trait on a struct that will serve as encryption context
///
///
/// The type of context data that will be passed to various encryption
/// function calls.
#[allow(unused_variables)]
pub trait KeyManager: Send + Sized {
    // type Context: Send;

    /// Get the latest version of a key ID. Return `VersionInvalid` if not found.
    fn get_latest_key_version(key_id: u32) -> Result<u32, KeyError>;

    /// Return a key for a key version and ID.
    ///
    /// Given a key ID and a version, write the key to the `key` buffer. If the
    /// buffer is too small, return [`KeyError::BufferTooSmall`].
    fn get_key(key_id: u32, key_version: u32, dst: &mut [u8]) -> Result<(), KeyError>;

    /// Calculate the length of a key. Usually this is constant, but the key ID
    /// and version can be taken into account if needed.
    ///
    /// On the C side, this function is combined with `get_key`.
    fn key_length(key_id: u32, key_version: u32) -> Result<usize, KeyError>;
}

pub trait Encryption {
    /// Initialize the encryption context object
    fn init(key_id: u32, key_version: u32, key: &[u8], iv: &[u8], flags: Flags) -> Self;

    /// Update the encryption context with new data, return the number of bytes
    /// written
    fn update(ctx: &mut Self, src: &[u8], dst: &mut [u8]) -> usize;

    /// Write the remaining bytes to the buffer
    fn finish(ctx: &mut Self, dst: &mut [u8]) -> usize;

    /// Return the exact length of the encrypted data based on the source length
    ///
    /// As this function must have a definitive answer, this API only supports
    /// encryption algorithms where this is possible to compute (i.e.,
    /// compression is not supported).
    fn encrypted_length(key_id: u32, key_version: u32, src_len: usize) -> usize;
}

// get_latest_key_version: #type::get_latest_key_version
// crypt_ctx_size: std::mem::size_of<#type::Context>()
// crypt_ctx_init: #type:init()
