//! Debug key management
//!
//! Use to debug the encryption code with a fixed key that changes only on user
//! request. The only valid key ID is 1.
//!
//! EXAMPLE ONLY: DO NOT USE IN PRODUCTION!

#![allow(unused)]

use std::cell::UnsafeCell;
use std::ffi::c_void;
use std::sync::atomic::{AtomicU32, AtomicUsize, Ordering};

use mariadb::plugin::encryption::{Encryption, Flags, KeyError, KeyManager};
use mariadb::plugin::{register_plugin, PluginType, SysVarOpt, *};

struct BasicKeyMgt;

static COUNTER: AtomicUsize = AtomicUsize::new(30);

impl Init for BasicKeyMgt {
    fn init() -> Result<(), InitError> {
        eprintln!("init for BasicKeyMgt");
        Ok(())
    }

    fn deinit() -> Result<(), InitError> {
        eprintln!("deinit for BasicKeyMgt");
        Ok(())
    }
}

impl KeyManager for BasicKeyMgt {
    fn get_latest_key_version(key_id: u32) -> Result<u32, KeyError> {
        eprintln!("get latest key version with {key_id}");
        static KCOUNT: AtomicU32 = AtomicU32::new(1);
        let ret = KCOUNT.fetch_add(1, Ordering::Relaxed);
        dbg!(ret);
        Ok(ret)
    }

    fn get_key(key_id: u32, key_version: u32, dst: &mut [u8]) -> Result<(), KeyError> {
        let s = format!("get_key: {key_id}:{key_version}");
        eprintln!("{s}, {}", dst.len());

        if dst.len() < dbg!(COUNTER.fetch_add(1, Ordering::Relaxed)) {
            return Err(KeyError::BufferTooSmall);
        }

        // Copy our slice to the buffer, return the copied length
        dst[..s.len()].copy_from_slice(s.as_str().as_bytes());
        Ok(())
    }

    fn key_length(key_id: u32, key_version: u32) -> Result<usize, KeyError> {
        eprintln!("get key length with {key_id}:{key_version}");
        Ok(COUNTER.load(Ordering::Relaxed))
    }
}

register_plugin! {
    BasicKeyMgt,
    ptype: PluginType::MariaEncryption,
    name: "basic_key_management",
    author: "Trevor Gross",
    description: "Basic key management plugin",
    license: License::Gpl,
    maturity: Maturity::Experimental,
    version: "0.1",
    init: BasicKeyMgt, // optional
    encryption: false,
}
