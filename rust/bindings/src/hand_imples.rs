use std::ffi::{c_char, c_int, c_uint, c_void};

use super::{mysql_var_check_func, mysql_var_update_func, TYPELIB};

// Defined in service_encryption.h but not imported because of tilde syntax
pub const ENCRYPTION_KEY_VERSION_INVALID: c_uint = !0;

// We hand write these stucts because the definition is tricky, not all fields are
// always present
#[repr(C)]
#[derive(Debug)]
pub struct st_mysql_sys_var_basic<T> {
    pub flags: c_int,
    pub name: *const c_char,
    pub comment: *const c_char,
    pub check: mysql_var_check_func,
    pub update: mysql_var_update_func,
    pub value: *mut T,
    pub def_val: T,
}

#[repr(C)]
#[derive(Debug)]
pub struct st_mysql_sys_var_const_basic<T> {
    pub flags: c_int,
    pub name: *const c_char,
    pub comment: *const c_char,
    pub check: mysql_var_check_func,
    pub update: mysql_var_update_func,
    pub value: *const T,
    pub def_val: T,
}

#[repr(C)]
#[derive(Debug)]
pub struct st_mysql_sys_var_simple<T> {
    pub flags: c_int,
    pub name: *const c_char,
    pub comment: *const c_char,
    pub check: mysql_var_check_func,
    pub update: mysql_var_update_func,
    pub value: *mut T,
    pub def_val: T,
    pub min_val: T,
    pub max_val: T,
    pub blk_sz: T,
}

#[repr(C)]
#[derive(Debug)]
pub struct st_mysql_sys_var_typelib<T> {
    pub flags: c_int,
    pub name: *const c_char,
    pub comment: *const c_char,
    pub check: mysql_var_check_func,
    pub update: mysql_var_update_func,
    pub value: *const T,
    pub def_val: T,
    pub typelib: TYPELIB,
}

type THDVAR_FUNC<T> = Option<unsafe extern "C" fn(thd: *const c_void, offset: c_int) -> *mut T>;

#[repr(C)]
#[derive(Debug)]
pub struct st_mysql_sys_var_thd_basic<T> {
    pub flags: c_int,
    pub name: *const c_char,
    pub comment: *const c_char,
    pub check: mysql_var_check_func,
    pub update: mysql_var_update_func,
    pub offset: c_int,
    pub def_val: T,
    pub resolve: THDVAR_FUNC<T>,
}

#[repr(C)]
#[derive(Debug)]
pub struct st_mysql_sys_var_thd_simple<T> {
    pub flags: c_int,
    pub name: *const c_char,
    pub comment: *const c_char,
    pub check: mysql_var_check_func,
    pub update: mysql_var_update_func,
    pub offset: c_int,
    pub def_val: T,
    pub min_val: T,
    pub max_val: T,
    pub blk_sz: T,
    pub resolve: THDVAR_FUNC<T>,
}

#[repr(C)]
#[derive(Debug)]
pub struct st_mysql_sys_var_thd_typelib<T> {
    pub flags: c_int,
    pub name: *const c_char,
    pub comment: *const c_char,
    pub check: mysql_var_check_func,
    pub update: mysql_var_update_func,
    pub offset: c_int,
    pub def_val: T,
    pub resolve: THDVAR_FUNC<T>,
    pub typelib: TYPELIB,
}
