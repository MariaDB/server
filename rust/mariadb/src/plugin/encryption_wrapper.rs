//! Wrappers needed for the `st_mariadb_encryption` type

use std::cmp::max;
use std::ffi::{c_int, c_uchar, c_uint, c_void};
use std::{mem, slice};

use log::{error, warn};
use mariadb_sys as bindings;

use super::encryption::{Decryption, Encryption, Flags, KeyError, KeyManager};

pub trait WrapKeyMgr: KeyManager {
    /// Get the key version, simple wrapper
    extern "C" fn wrap_get_latest_key_version(key_id: c_uint) -> c_uint {
        match Self::get_latest_key_version(key_id) {
            Ok(v) => {
                if v == bindings::ENCRYPTION_KEY_NOT_ENCRYPTED {
                    error!("get_latest_key_version returned value {v}, which is reserved for unencrypted \
                            keys. The server will likely restart.");
                } else if v == bindings::ENCRYPTION_KEY_VERSION_INVALID {
                    panic!("get_latest_key_version returned value {v}, which is reserved for invalid keys. \
                            Return an Err if this is intended.");
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
        if dstbuf.is_null() {
            match Self::key_length(key_id, version) {
                // FIXME: don't unwrap
                Ok(v) => *buflen = v.try_into().unwrap(),
                Err(e) => {
                    return e as c_uint;
                }
            }
            return bindings::ENCRYPTION_KEY_BUFFER_TOO_SMALL;
        }

        // SAFETY: caller guarantees validity
        let buf = slice::from_raw_parts_mut(dstbuf, (*buflen).try_into().unwrap());

        // If successful, return 0. If an error occurs, return it
        match Self::get_key(key_id, version, buf) {
            Ok(()) => 0,
            Err(e) => e as c_uint,
        }
    }
}

impl<T> WrapKeyMgr for T where T: KeyManager {}

/// Store whether we are encrypting or decrypting
#[repr(C)]
enum CryptCtxWrapper<En, De> {
    Encrypt(En),
    Decrypt(De),
}

/// Ctx needs to be as big as the largest needed for either encryption or decryption, since there isn't
/// a way to discern in the plugin API
pub extern "C" fn wrap_crypt_ctx_size<En: Encryption, De: Decryption>(
    _key_id: c_uint,
    _key_version: c_uint,
) -> c_uint {
    // CHECKME might define how many bytes extra we get on the buffer
    // I believe that key_id and key_version are provided in case this plugin
    // uses different structs for different keys. However, it seems safer & more
    // user friendly to sidestep that and just make everything the same size
    mem::size_of::<CryptCtxWrapper<En, De>>()
        .try_into()
        .unwrap()
}

/// # Safety
///
/// The caller must guarantee that the following is tre
///
/// - `ctx` points to memory with the size of T (may be uninitialized)
/// - `key` exists for `klen`
/// - `iv` exists for `ivlen`
pub unsafe extern "C" fn wrap_crypt_ctx_init<En: Encryption, De: Decryption>(
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
    let keybuf = slice::from_raw_parts(key, klen.try_into().unwrap());
    let ivbuf = slice::from_raw_parts(iv, ivlen.try_into().unwrap());
    let flags = Flags::new(flags);

    let init_res = if flags.should_encrypt() {
        En::init(key_id, key_version, keybuf, ivbuf, flags).map(CryptCtxWrapper::Encrypt)
    } else {
        De::init(key_id, key_version, keybuf, ivbuf, flags).map(CryptCtxWrapper::Decrypt)
    };

    let newctx = match init_res {
        Ok(c) => c,
        Err(e) => return e as c_int,
    };

    ctx.cast::<CryptCtxWrapper<En, De>>().write(newctx);
    bindings::MY_AES_OK.try_into().unwrap()
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
pub unsafe extern "C" fn wrap_crypt_ctx_update<En: Encryption, De: Decryption>(
    ctx: *mut c_void,
    src: *const c_uchar,
    slen: c_uint,
    dst: *mut c_uchar,
    dlen: *mut c_uint,
) -> c_int {
    // debug_assert!(*dlen >= slen, "using a version from before MDEV-30309");
    let sbuf = slice::from_raw_parts(src, slen.try_into().unwrap());
    let dbuf = slice::from_raw_parts_mut(dst, slen.try_into().unwrap());
    let this: &mut CryptCtxWrapper<En, De> = &mut *ctx.cast();

    let update_res = match this {
        CryptCtxWrapper::Encrypt(v) => v.update(sbuf, dbuf),
        CryptCtxWrapper::Decrypt(v) => v.update(sbuf, dbuf),
    };

    let (ret, written) = match update_res {
        Ok(v) => (
            bindings::MY_AES_OK.try_into().unwrap(),
            v.try_into().unwrap(),
        ),
        Err(e) => (e as c_int, 0),
    };

    *dlen = written;
    ret
}

pub unsafe extern "C" fn wrap_crypt_ctx_finish<En: Encryption, De: Decryption>(
    ctx: *mut c_void,
    dst: *mut c_uchar,
    dlen: *mut c_uint,
) -> c_int {
    let dbuf = slice::from_raw_parts_mut(dst, (*dlen).try_into().unwrap());
    let this: &mut CryptCtxWrapper<En, De> = &mut *ctx.cast();

    let finish_res = match this {
        CryptCtxWrapper::Encrypt(v) => v.finish(dbuf),
        CryptCtxWrapper::Decrypt(v) => v.finish(dbuf),
    };

    let (ret, written) = match finish_res {
        Ok(v) => (
            bindings::MY_AES_OK.try_into().unwrap(),
            v.try_into().unwrap(),
        ),
        Err(e) => (e as c_int, 0),
    };

    *dlen = written;
    // Destroy the context
    ctx.drop_in_place();
    ret
}

pub unsafe extern "C" fn wrap_encrypted_length<En: Encryption>(
    slen: c_uint,
    key_id: c_uint,
    key_version: c_uint,
) -> c_uint {
    En::encrypted_length(key_id, key_version, slen.try_into().unwrap())
        .try_into()
        .unwrap()
}

unsafe fn set_buflen_with_check(buflen: *mut c_uint, val: u32) {
    if val > 32 {
        eprintln!(
            "The default encryption does not seem to allow keys above 32 bits. If the server \
            crashes after this message, that is the likely error"
        );
    }
    *buflen = val.try_into().unwrap();
}
