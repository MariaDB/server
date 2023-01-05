//! Proc macro plugin! to register a plugin
//!
//! Desired usage:
//! ```
//! plugin_encryption!{
//!     type: RustEncryption,
//!     init: RustEncryptionInit, // optional
//!     keymgr: RustEncryptionInit, // optional
//!     encryption: RustEncryptionInit, // optional
//!     name: "example_key_management",
//!     author: "MariaDB Team",
//!     description: "Example key management plugin using AES",
//!     license: GPL,
//!     stability: EXPERIMENTAL
//! }
//! ```
//!
//! Desired output:
//!
//! ```
//! bindings::st_maria_plugin {
//! type_: ptype as c_int,
//! info: *mut c_void,
//! name: cstr::cstr!($name).as_ptr(),
//! author: cstr::cstr!($author).as_ptr(),
//! descr: cstr::cstr!($description).as_ptr(),
//! license: c_int,
//! init: Option<unsafe extern "C" fn(arg1: *mut c_void) -> c_int>,
//! deinit: Option<unsafe extern "C" fn(arg1: *mut c_void) -> c_int>,
//! version: vers as c_uint,
//! status_vars: *mut st_mysql_show_var,
//! system_vars: *mut *mut st_mysql_sys_var,
//! version_info: plugin!(
//!     @def
//!     cstr::cstr!($vers).as_ptr(),
//!     $(cstr::cstr!($comment).as_ptr())?
//! ),,
//! maturity: maturity as c_uint,
//! }
//! ```

use proc_macro::TokenStream;

pub fn entry(item: TokenStream) -> TokenStream {
    dbg!(&item);
    item
}
