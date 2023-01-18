//! Wrappers needed for the `st_mariadb_encryption` type

use std::ffi::{c_int, c_uchar, c_uint, c_void};
use std::{mem, slice};

use mariadb_sys as bindings;

use super::encryption::{Encryption, Flags, KeyError, KeyManager};
use crate::{error, warn};

///
pub trait WrapKeyMgr: KeyManager {
    /// Get the key version, simple wrapper
    extern "C" fn wrap_get_latest_key_version(key_id: c_uint) -> c_uint {
        match Self::get_latest_key_version(key_id) {
            Ok(v) => {
                if v == bindings::ENCRYPTION_KEY_NOT_ENCRYPTED {
                    error!(target: "KeyManager", "get_latest_key_version returned value {v}, which is reserved for unencrypted keys.");
                    error!(target: "KeyManager", "the server will likely shut down now.");
                }
                v
            }
            Err(_) => KeyError::VersionInvalid as c_uint,
        }
    }

    /// If key == NULL, return the required buffer size for the key
    ///
    /// # Safety
    ///
    /// `dstbuf` must be valid for `buflen`
    unsafe extern "C" fn wrap_get_key(
        key_id: c_uint,
        version: c_uint,
        dstbuf: *mut c_uchar,
        buflen: *mut c_uint,
    ) -> c_uint {
        dbg!(key_id, version, dstbuf, *buflen);
        if dstbuf.is_null() {
            match dbg!(Self::key_length(key_id, version)) {
                // FIXME: don't unwrap
                Ok(v) => *buflen = v.try_into().unwrap(),
                Err(e) => {
                    return e as c_uint;
                }
            }
            return bindings::ENCRYPTION_KEY_BUFFER_TOO_SMALL;
        }

        // SAFETY: caller guarantees validity
        let buf = slice::from_raw_parts_mut(dstbuf, *buflen as usize);

        // If successful, return 0. If an error occurs, return it
        match dbg!(Self::get_key(key_id, version, buf)) {
            Ok(_) => 0,
            Err(e) => {
                dbg!(e);

                // match e {
                //     // Set the desired buffer size if available
                //     KeyError::BufferTooSmall => {
                //         *buflen = dbg!(Self::key_length(key_id, version)
                //             .unwrap_or(0)
                //             .try_into()
                //             .unwrap())
                //     }
                //     _ => (),
                // }
                dbg!(e as c_uint)
            }
        }
    }
}

impl<T> WrapKeyMgr for T where T: KeyManager {}
impl<T> WrapEncryption for T where T: Encryption {}

pub trait WrapEncryption: Encryption {
    extern "C" fn wrap_crypt_ctx_size(_key_id: c_uint, _key_version: c_uint) -> c_uint {
        // I believe that key_id and key_version are provided in case this plugin
        // uses different structs for different keys. However, it seems safer & more
        // user friendly to sidestep that and just make everything the same size
        mem::size_of::<Self>().try_into().unwrap()
    }

    /// # Safety
    ///
    /// The caller must guarantee that the following is tre
    ///
    /// - `ctx` points to memory with the size of T (may be uninitialized)
    /// - `key` exists for `klen`
    /// - `iv` exists for `ivlen`
    unsafe extern "C" fn wrap_crypt_ctx_init(
        ctx: *mut c_void,
        key: *const c_uchar,
        klen: c_uint,
        iv: *const c_uchar,
        ivlen: c_uint,
        flags: c_int,
        key_id: c_uint,
        key_version: c_uint,
    ) -> c_int {
        /// SAFETY: caller guarantees buffer validity
        let keybuf = slice::from_raw_parts(key, klen as usize);
        let ivbuf = slice::from_raw_parts(iv, ivlen as usize);
        let flags = Flags::new(flags);
        match Self::init(key_id, key_version, keybuf, ivbuf, flags) {
            Ok(newctx) => {
                ctx.cast::<Self>().write(newctx);
                bindings::MY_AES_OK.try_into().unwrap()
            }
            Err(e) => e as c_int,
        }
    }

    /// # Safety
    ///
    /// The caller must guarantee that the following is true:
    ///
    /// - `ctx` points to a valid, initialized object of type T
    /// - `src` exists for `slen`
    /// - ~~`dst` exists for `dlen`~~
    ///
    /// FIXME: the `*dlen` we receive from the server is unitialized. For now we
    /// assume the destination buffer is equal to source buffer length, but this
    /// is a bit of a workaround until MDEV-30389 is resolved.
    unsafe extern "C" fn wrap_crypt_ctx_update(
        ctx: *mut c_void,
        src: *const c_uchar,
        slen: c_uint,
        dst: *mut c_uchar,
        dlen: *mut c_uint,
    ) -> c_int {
        // dbg!(slen, dlen, *dlen);
        let sbuf = slice::from_raw_parts(src, slen as usize);
        let dbuf = slice::from_raw_parts_mut(dst, slen as usize);

        let c: &mut Self = &mut *ctx.cast();
        let (ret, written) = match c.update(sbuf, dbuf) {
            // FIXME dlen
            Ok(_) => (bindings::MY_AES_OK.try_into().unwrap(), 0),
            Err(e) => (e as c_int, 0),
        };
        *dlen = written;
        ret
    }

    unsafe extern "C" fn wrap_crypt_ctx_finish(
        ctx: *mut c_void,
        dst: *mut c_uchar,
        dlen: *mut c_uint,
    ) -> c_int {
        dbg!(*dlen);
        let dbuf = slice::from_raw_parts_mut(dst, dlen as usize);

        let c: &mut Self = &mut *ctx.cast();
        let (ret, written) = match c.finish(dbuf) {
            // FIXME dlen
            Ok(_) => (bindings::MY_AES_OK.try_into().unwrap(), 0),
            Err(e) => (e as c_int, 0),
        };

        ctx.drop_in_place();
        ret
    }

    unsafe extern "C" fn wrap_encrypted_length(
        slen: c_uint,
        key_id: c_uint,
        key_version: c_uint,
    ) -> c_uint {
        Self::encrypted_length(key_id, key_version, slen.try_into().unwrap())
            .try_into()
            .unwrap()
    }
}

unsafe fn set_buflen_with_check(buflen: *mut c_uint, val: u32) {
    if val > 32 {
        eprintln!(
            "The default encryption does not seem to allow keys above 32 bits. If the server \
            crashes after this message, that is the likely error"
        );
    }
    *buflen = val.try_into().unwrap()
}
