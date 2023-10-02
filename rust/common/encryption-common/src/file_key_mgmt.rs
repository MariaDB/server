//! File key managment common elements that we can reuse
//!
//! Parses a file like:
//!
//! ```text
//! 1;a7addd9adea9978fda19f21e6be987880e68ac92632ca052e5bb42b1a506939a
//! 2;49c16acc2dffe616710c9ba9a10b94944a737de1beccb52dc1560abfdd67388b
//! 100;8db1ee74580e7e93ab8cf157f02656d356c2f437d548d5bf16bf2a56932954a3
//! key_id;hexified_key
//! ```
//!
//! See <https://mariadb.com/kb/en/file-key-management-encryption-plugin/> for
//! more details.

use std::collections::BTreeMap;
use std::num::ParseIntError;
use std::path::Path;
use std::{fmt, fs, io};

pub const MAX_KEY_LEN: usize = 32;
pub type KeyMap = BTreeMap<u32, Key>;

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Key {
    key: [u8; MAX_KEY_LEN],
    len: usize,
}

impl Key {
    pub fn as_buf(&self) -> &[u8] {
        &self.key[..self.len]
    }
}

pub fn read_from_file(path: impl AsRef<Path>) -> Result<KeyMap, Error> {
    log::info!("Reading key file at '{}'", path.as_ref().display());
    let contents = fs::read_to_string(path)?;
    let mut ret = BTreeMap::new();

    for line in contents.lines() {
        if line.starts_with('#') {
            continue;
        }
        let (id, key_hex) = line
            .split_once(';')
            .ok_or_else(|| Error::LineSyntax(line.into()))?;
        let key_id: u32 = id.parse().map_err(|e| Error::Int(e, id.into()))?;
        let key_hex = key_hex.trim();
        if key_hex.len() > MAX_KEY_LEN * 2 {
            return Err(Error::KeyTooLong(key_hex.into()));
        }
        let mut key = Key {
            key: [0u8; MAX_KEY_LEN],
            // `hex` checks for invalid keys
            len: key_hex.len() >> 1,
        };

        hex::decode_to_slice(key_hex, &mut key.key[..key.len])
            .map_err(|e| Error::Hex(e, key_hex.into()))?;
        ret.insert(key_id, key);
    }
    Ok(ret)
}

#[derive(Debug)]
pub enum Error {
    Io(io::Error),
    /// Got an invalid line
    LineSyntax(Box<str>),
    Hex(hex::FromHexError, Box<str>),
    Int(ParseIntError, Box<str>),
    KeyTooLong(Box<str>),
}

impl From<io::Error> for Error {
    fn from(value: io::Error) -> Self {
        Self::Io(value)
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Io(e) => write!(f, "io error: {e}"),
            Error::LineSyntax(v) => write!(f, "invalid line at '{v}'"),
            Error::Hex(e, v) => write!(f, "error decoding hex at '{v}': {e}"),
            Error::Int(e, v) => write!(f, "error parsing int at '{v}': {e}"),
            Error::KeyTooLong(v) => {
                write!(f, "key too long at '{v}', maximum length {MAX_KEY_LEN}")
            }
        }
    }
}

/// We may want to use file key managment with multiple plugins. Gets messy with statics, so
/// we just dump it into a macro.
///
/// This macro creates two things that need to be registered: the `KeyManager` struct `$tyname`
/// and a global `SysVarConstString` `$file_ident`.
#[macro_export]
macro_rules! create_fkmgt {
    ($tyname:ident, $file_ident:ident) => {
        use mariadb::plugin::encryption::KeyError as _MacroKeyError;

        /// Name of the config file
        static $file_ident: mariadb::plugin::SysVarConstString =
            mariadb::plugin::SysVarConstString::new();
        /// Map of the keys
        static KEYS: std::sync::OnceLock<$crate::file_key_mgmt::KeyMap> =
            std::sync::OnceLock::new();

        struct $tyname;

        impl mariadb::plugin::Init for $tyname {
            fn init() -> Result<(), mariadb::plugin::InitError> {
                let path = $file_ident.get();
                match $crate::file_key_mgmt::read_from_file(&path) {
                    Ok(map) => {
                        KEYS.set(map).unwrap();
                        Ok(())
                    }
                    Err(e) => {
                        mariadb::log::error!("failed to load keyfile at '{path}': {e}");
                        Err(mariadb::plugin::InitError)
                    }
                }
            }
        }

        impl mariadb::plugin::encryption::KeyManager for FileKeyMgmt {
            fn get_latest_key_version(key_id: u32) -> Result<u32, _MacroKeyError> {
                let _ = get_key_from_id(key_id, 1)?;
                Ok(1)
            }

            fn get_key(
                key_id: u32,
                key_version: u32,
                dst: &mut [u8],
            ) -> Result<(), _MacroKeyError> {
                let key = get_key_from_id(key_id, key_version)?;
                let keybuf = key.as_buf();
                if dst.len() < keybuf.len() {
                    mariadb::log::error!(
                        "got buffer of length {} but require {} bytes",
                        keybuf.len(),
                        dst.len()
                    );
                    return Err(_MacroKeyError::BufferTooSmall);
                }
                dst[..keybuf.len()].copy_from_slice(keybuf);
                Ok(())
            }

            fn key_length(key_id: u32, key_version: u32) -> Result<usize, _MacroKeyError> {
                let key = get_key_from_id(key_id, key_version)?;
                Ok(key.as_buf().len())
            }
        }

        fn get_key_from_id(
            key_id: u32,
            key_version: u32,
        ) -> Result<$crate::file_key_mgmt::Key, _MacroKeyError> {
            if key_version != 1 {
                mariadb::log::error!("invalid key version {key_version}");
                return Err(_MacroKeyError::InvalidVersion);
            }
            let Some(key) = KEYS.get().unwrap().get(&key_id) else {
                mariadb::log::error!("invalid key ID {key_id}");
                return Err(_MacroKeyError::InvalidVersion);
            };
            Ok(*key)
        }
    };
}
