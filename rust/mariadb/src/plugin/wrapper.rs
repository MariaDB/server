use std::cell::UnsafeCell;
use std::ffi::{c_int, c_uint, c_void};
use std::ptr;

use super::{Init, License, Maturity, PluginType};
use crate::bindings;

/// Used for plugin registrations, which are in global scope.
#[doc(hidden)]
#[derive(Debug)]
#[repr(transparent)]
pub struct UnsafeSyncCell<T>(UnsafeCell<T>);

impl<T> UnsafeSyncCell<T> {
    /// # Safety
    ///
    /// This inner value be used in a Sync/Send way
    pub const unsafe fn new(value: T) -> Self {
        Self(UnsafeCell::new(value))
    }

    pub const fn as_ptr(&self) -> *const T {
        self.0.get()
    }

    pub const fn get(&self) -> *mut T {
        self.0.get()
    }

    pub fn get_mut(&mut self) -> &mut T {
        self.0.get_mut()
    }
}

unsafe impl<T> Send for UnsafeSyncCell<T> {}
unsafe impl<T> Sync for UnsafeSyncCell<T> {}

/// Trait for easily wrapping init/deinit functions
pub trait WrapInit: Init {
    #[must_use]
    unsafe extern "C" fn wrap_init(_: *mut c_void) -> c_int {
        match Self::init() {
            Ok(_) => 0,
            Err(_) => 1,
        }
    }

    #[must_use]
    unsafe extern "C" fn wrap_deinit(_: *mut c_void) -> c_int {
        match Self::deinit() {
            Ok(_) => 0,
            Err(_) => 1,
        }
    }
}

impl<T> WrapInit for T where T: Init {}

/// New struct with all null values
#[must_use]
#[doc(hidden)]
pub const fn new_null_st_maria_plugin() -> bindings::st_maria_plugin {
    bindings::st_maria_plugin {
        type_: 0,
        info: ptr::null_mut(),
        name: ptr::null(),
        author: ptr::null(),
        descr: ptr::null(),
        license: 0,
        init: None,
        deinit: None,
        version: 0,
        status_vars: ptr::null_mut(),
        system_vars: ptr::null_mut(),
        version_info: ptr::null(),
        maturity: 0,
    }
}
