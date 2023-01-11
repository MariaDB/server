use std::ffi::{c_void,c_int};

pub unsafe extern "C" fn wrap_init<T:Init>(_: *const c_void) -> c_int {
    T::init()
}

pub unsafe extern "C" fn wrap_deinit<T:Init>(_: *const c_void) -> c_int {
    T::deinit()
}
