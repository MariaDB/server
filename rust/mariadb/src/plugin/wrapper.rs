use std::ffi::{c_int, c_uint, c_void};
use std::{env, ptr};

use super::{Init, InitError, License, Maturity, PluginType};
use crate::{bindings, configure_logger};

/// Meta that we generate in the proc macro, which we can use to get information about our type in
/// wrappers
pub trait PluginMeta {
    const NAME: &'static str;
}

/// Wrap the init call
#[must_use]
pub unsafe extern "C" fn wrap_init_fn<P: PluginMeta, I: Init>(_: *mut c_void) -> c_int {
    init_common();
    let ret = match I::init() {
        Ok(()) => 0,
        Err(InitError) => 1,
    };
    log::info!("loaded plugin {}", P::NAME);
    ret
}

/// Wrap the deinit call
#[must_use]
pub unsafe extern "C" fn wrap_deinit_fn<P: PluginMeta, I: Init>(_: *mut c_void) -> c_int {
    let ret = match I::deinit() {
        Ok(()) => 0,
        Err(InitError) => 1,
    };
    log::info!("unloaded plugin {}", P::NAME);
    ret
}

/// Init call for plugins that don't provide a custom init function
#[must_use]
pub unsafe extern "C" fn default_init_notype<P: PluginMeta>(_: *mut c_void) -> c_int {
    init_common();
    log::info!("loaded plugin {}", P::NAME);
    0
}

#[must_use]
pub unsafe extern "C" fn default_deinit_notype<P: PluginMeta>(_: *mut c_void) -> c_int {
    log::info!("unloaded plugin {}", P::NAME);
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
