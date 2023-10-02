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
use mariadb_sys as bindings;

/// A type of error to be used by key functions
#[repr(u32)]
#[non_exhaustive]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum KeyError {
    // Values must be nonzero
    /// A key ID is invalid or not found. Maps to `ENCRYPTION_KEY_VERSION_INVALID` in C.
    InvalidVersion = bindings::ENCRYPTION_KEY_VERSION_INVALID,
    /// A key buffer is too small. Maps to `ENCRYPTION_KEY_BUFFER_TOO_SMALL` in C.
    BufferTooSmall = bindings::ENCRYPTION_KEY_BUFFER_TOO_SMALL,
    Other = 3,
}

#[repr(i32)]
#[non_exhaustive]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EncryptionError {
    BadData = bindings::MY_AES_BAD_DATA,
    BadKeySize = bindings::MY_AES_BAD_KEYSIZE,
    /// Generic error; return this for e.g. insufficient data length
    Other = bindings::MY_AES_OPENSSL_ERROR,
}

/// Representation of the flags integer
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Flags(i32);

/// The possible action from
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Action {
    Encrypt,
    Decrypt,
}

impl Flags {
    pub(crate) const fn new(value: i32) -> Self {
        Self(value)
    }

    /// Return whether or not
    pub const fn action(self) -> Action {
        if (self.0 & bindings::ENCRYPTION_FLAG_ENCRYPT as i32) == 0 {
            Action::Decrypt
        } else {
            Action::Encrypt
        }
    }

    /// True if encryption is disallowed from appending extra data (no AEADs)
    pub const fn nopad(&self) -> bool {
        (self.0 & bindings::ENCRYPTION_FLAG_NOPAD as i32) != 0
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
    ///
    /// This cannot return key version of 0 or the server will think something is wrong.
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

/// Implement this on a type that can encrypt data
pub trait Encryption: Sized {
    /// Initialize the encryption context object.
    ///
    /// Parameters:
    ///
    /// - `key`: the key to use for encryption
    /// - `iv`: the initialization vector (nonce) to be used for encryption
    /// - `same_size`: if `true`, the `src` and `dst` length will always be the same. That is,
    ///    ciphers cannot add additional data. The default implementation uses this to select
    ///    between an AEAD (AES-256-GCM) if additional data is allowed, and a streaming cipher
    ///    (AES-CBC) when the
    /// -  `key_id` and `key_version`: these can be used if encryption depends on key information.
    ///    Note that `key` may not be exactly the same as the result of `KeyManager::get_key`.
    fn init(
        key_id: u32,
        key_version: u32,
        key: &[u8],
        iv: &[u8],
        same_size: bool,
    ) -> Result<Self, EncryptionError>;

    /// Update the encryption context with new data, return the number of bytes
    /// written.
    ///
    /// Do not append the iv to the ciphertext, MariaDB keeps track of it separately.
    fn update(&mut self, src: &[u8], dst: &mut [u8]) -> Result<usize, EncryptionError>;

    /// Finish encryption. Usually this performs validation and, in some cases, can be used to
    /// write additional data.
    ///
    /// If init was called with `same_size = true`, `dst` will likely be empty.
    fn finish(&mut self, dst: &mut [u8]) -> Result<usize, EncryptionError> {
        Ok(0)
    }

    /// Return the exact length of the encrypted data based on the source length. Defaults to
    /// the same value.
    ///
    /// As this function must have a definitive answer, this API only supports
    /// encryption algorithms where this is possible to compute (i.e.,
    /// compression is not supported).
    ///
    /// Note that if initialization was called with `same_size = true`, this will be ignored. In
    /// that case.
    fn encrypted_length(key_id: u32, key_version: u32, src_len: usize) -> usize {
        src_len
    }
}

/// Implement this on a type that decrypts data.
///
/// This can be the same type as [`Encryption`] but does not have to be.
pub trait Decryption: Sized {
    /// Initialize the decryption context object. See [`Encryption::init`] for information on
    /// parameters.
    fn init(
        key_id: u32,
        key_version: u32,
        key: &[u8],
        iv: &[u8],
        same_size: bool,
    ) -> Result<Self, EncryptionError>;

    /// Update the encryption context with new data, return the number of bytes
    /// written.
    fn update(&mut self, src: &[u8], dst: &mut [u8]) -> Result<usize, EncryptionError>;

    /// Finish decryption. Usually this performs validation and, in some cases, can be used to
    /// write additional data.
    ///
    /// If init was called with `same_size = true`, `dst` will likely be empty.
    fn finish(&mut self, dst: &mut [u8]) -> Result<usize, EncryptionError> {
        Ok(0)
    }
}
