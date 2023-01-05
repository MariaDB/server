//! Wrappers needed for the `st_mariadb_encryption` type

use mariadb_server_sys as bindings;
use std::{
    ffi::{c_int, c_uchar, c_uint, c_void},
    slice,
};

use super::encryption::{KeyError, KeyManager};

/// Get the key version, simple wrapper
pub unsafe extern "C" fn wrap_get_latest_key_version<T: KeyManager>(key_id: c_uint) -> c_uint {
    match T::get_latest_key_version(key_id) {
        Ok(v) => v,
        Err(_) => KeyError::VersionInvalid as c_uint,
    }
}

/// If key == NULL, return the required buffer size for the key
///
///
pub unsafe extern "C" fn wrap_get_key<T: KeyManager>(
    key_id: c_uint,
    version: c_uint,
    dstbuf: *mut c_uchar,
    buflen: *mut c_uint,
) -> c_uint {
    if dstbuf.is_null() {
        match T::key_length(key_id, version) {
            // FIXME: don't unwrap
            Ok(v) => *buflen = v.try_into().unwrap(),
            Err(e) => {
                return e as c_uint;
            }
        }
        return bindings::ENCRYPTION_KEY_BUFFER_TOO_SMALL;
    }

    // SAFETY: caller guarantees validity
    let buf = slice::from_raw_parts_mut(dstbuf, buflen as usize);

    // If successful, return 0. If an error occurs, return it
    match T::get_key(key_id, version, buf) {
        Ok(_) => 0,
        Err(e) => e as c_uint,
    }
}

pub unsafe extern "C" fn wrap_crypt_ctx_size<T>(key_id: c_uint, key_version: c_uint) -> c_uint {
    todo!()
}

pub unsafe extern "C" fn wrap_crypt_ctx_init<T>(
    ctx: *mut c_void,
    key: *const c_uchar,
    klen: c_uint,
    iv: *const c_uchar,
    ivlen: c_uint,
    flags: c_int,
    key_id: c_uint,
    key_version: c_uint,
) -> c_int {
    todo!()
}

pub unsafe extern "C" fn wrap_crypt_ctx_update<T>(
    ctx: *mut c_void,
    src: *const c_uchar,
    slen: c_uint,
    dst: *mut c_uchar,
    dlen: *mut c_uint,
) -> c_int {
    todo!()
}

pub unsafe extern "C" fn wrap_crypt_ctx_finish<T>(
    ctx: *mut c_void,
    dst: *mut c_uchar,
    dlen: *mut c_uint,
) -> c_int {
    todo!()
}

pub unsafe extern "C" fn wrap_encrypted_length<T>(
    slen: c_uint,
    key_id: c_uint,
    key_version: c_uint,
) -> c_uint {
    todo!()
}
