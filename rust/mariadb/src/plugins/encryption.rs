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


/// Struct representing an invalid version error for `get_latest_key_version`
pub struct VersionInvalid;

#[repr(u32)]
#[non_exhaustive]
pub enum KeyError {
    VersionInvalid,
    BufferTooSmall,
    Other
}

// API that allows safely writing to a memory location
pub struct BufVec {
    buf: &mut [u8]
    len: usize
}

impl BufVec {
    fn try_push()
}

/// Implement this trait on a struct that will serve as encryption context
///
///
pub trait Encryption {
    /// Get the latest version of a key ID
    fn get_latest_key_version(key_id: u32) -> Result<u32, VersionInvalid>;

    /// Given a key ID and a 
    fn get_key(key_id: u32, version: u32, key: &mut BufVec)  -> Result<(), EncryptionError> 

    /// Initialize
    fn init(key: &[u8], iv: &[u8], flags: u32, key_id: u32, ) -> Self;

    /// Initialize
    fn update(&mut self, src: &[u8], dst: &mut BufVec);

    fn finish(&mut self, dst: DestBuf)

    fn size(key_id: u32, key_version: u32) -> u32;

    fn encrypted_length(len: u32, )
    
}

// get_latest_key_version: #type::get_latest_key_version
// crypt_ctx_size: std::mem::size_of<#type>()
// crypt_ctx_init: #type:init()
