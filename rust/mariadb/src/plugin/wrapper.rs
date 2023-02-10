use std::ffi::{c_int, c_uint, c_void};
use std::{env, ptr};

use super::{Init, License, Maturity, PluginType};
use crate::{bindings, configure_logger};

/// Trait for easily wrapping init/deinit functions
pub trait WrapInit: Init {
    #[must_use]
    unsafe extern "C" fn wrap_init(_: *mut c_void) -> c_int {
        init_common();
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

/// Init call for plugins that don't provide a custom init function
#[inline]
pub unsafe extern "C" fn wrap_init_notype(_: *mut c_void) -> c_int {
    init_common();
    0
}

/// What to run when every plugin is loaded.
fn init_common() {
    configure_logger!();
    env::set_var("RUST_BACKTRACE", "1");
}

/// New struct with all null values
#[must_use]
#[doc(hidden)]
pub const fn new_null_plugin_st() -> bindings::st_maria_plugin {
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
