use super::Init;
use std::ffi::{c_int, c_void};

pub unsafe extern "C" fn wrap_init<T: Init>(_: *mut c_void) -> c_int {
    match T::init() {
        Ok(_) => 0,
        Err(_) => 1,
    }
}

pub unsafe extern "C" fn wrap_deinit<T: Init>(_: *mut c_void) -> c_int {
    match T::deinit() {
        Ok(_) => 0,
        Err(_) => 1,
    }
}
