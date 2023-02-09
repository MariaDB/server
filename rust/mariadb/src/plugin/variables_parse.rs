//! Variables parser
//!
//! Reimplementation of `check_func_x` in `sql_plugin.cc`. It's just easier to
//! reimpelment these because it means we can use thread safe types.
//!
//! All these functions share similar signatures, see `plugin.h`
//!
//! # Check functions
//!
//! Parse input
//!
//! - `thd`: thread handle
//! - `var`: system variable union. SAFETY: must be correct type (caller varifies)
//! - `save`: pointer to temporary storage
//! - `value`: user-provided value
//!
//! # Update function
//!
//!
//!
//! - `thd`: thread handle
//! - `var`: system variable union. SAFETY: must be correct type (caller varifies)
//! - `save`: pointer to temporary storage
//! - `value`: user-provided value

use std::cell::UnsafeCell;
use std::ffi::{c_int, c_void, CStr};
use std::os::raw::c_char;
use std::sync::atomic::{AtomicBool, Ordering};

use super::variables::SysVarInfoU;
use crate::bindings;
use crate::helpers::str2bool;

/// # Safety
///
/// Variable has to be of the right type, bool
pub unsafe fn check_func_atomic_bool<T>(
    thd: *const c_void,
    var: *mut c_void,
    // var: *mut SysVarInfoU<bool>,
    save: *mut c_void,
    value: *const bindings::st_mysql_value,
) -> c_int {
    todo!()
    // let sql_val = MySqlValue::from_ptr(value);
    // let dest: *const AtomicBool = save.cast();
    // let new_val = match sql_val.value() {
    //     Value::Int(v) => {
    //         let tmp = v.unwrap_or(0);
    //         match tmp {
    //             0 => false,
    //             1 => true,
    //             _ => return 1,
    //         }
    //     }
    //     Value::String(s) => {
    //         let inner = s.expect("got null string");
    //         str2bool(&inner)
    //             .unwrap_or_else(|| panic!("value '{inner}' is not a valid bool indicator"))
    //     }
    //     Value::Real(_) => panic!("unexpected real value"),
    // };
    // (*dest).store(new_val, Ordering::Relaxed);
    // 0
}

// pub unsafe fn update_func_atomic_bool<T>(
//     thd: *const c_void,
//     // var: *mut SysVarInfoU<i32>,
//     var: *mut c_void,
//     var_ptr: *mut c_void,
//     save: *const c_void,
// ) {
//     let dest: *const AtomicBool = var_ptr.cast();
//     let new_val: u8 = *save.cast();
//     let new_val_bool = match new_val {
//         1 => true,
//         0 => false,
//         n => panic!("invalid boolean value {n}"),
//     };
//     (*dest).store(new_val_bool, Ordering::Relaxed);
// }

#[derive(Debug, PartialEq)]
pub enum CliValue {
    Int(Option<i64>),
    Real(Option<f64>),
    String(Option<String>),
}

pub struct CliMysqlValue(UnsafeCell<bindings::st_mysql_value>);

impl CliMysqlValue {
    /// `item_val_str function`, `item_val_int`, `item_val_real`
    pub(crate) fn value(&self) -> CliValue {
        unsafe {
            match (*self.0.get()).value_type.unwrap()(self.0.get())
                .try_into()
                .unwrap()
            {
                bindings::MYSQL_VALUE_TYPE_INT => self.as_int(),
                bindings::MYSQL_VALUE_TYPE_REAL => self.as_real(),
                bindings::MYSQL_VALUE_TYPE_STRING => self.as_string(),
                x => panic!("unrecognized value type {x}"),
            }
        }
    }

    const unsafe fn from_ptr<'a>(ptr: *const bindings::st_mysql_value) -> &'a Self {
        &*ptr.cast()
    }

    unsafe fn as_int(&self) -> CliValue {
        let mut res = 0i64;
        let nul = (*self.0.get()).val_int.unwrap()(self.0.get(), &mut res);
        if nul == 0 {
            CliValue::Int(Some(res))
        } else {
            CliValue::Int(None)
        }
    }

    unsafe fn as_real(&self) -> CliValue {
        let mut res = 0f64;
        let nul = (*self.0.get()).val_real.unwrap()(self.0.get(), &mut res);
        if nul == 0 {
            CliValue::Real(Some(res))
        } else {
            CliValue::Real(None)
        }
    }

    unsafe fn as_string(&self) -> CliValue {
        let mut buf = vec![0u8; 512];
        let mut len: c_int = buf.len().try_into().unwrap();
        // This function copies ot the buffer if it fits, returns a temp
        // string otherwisw
        let ptr = (*self.0.get()).val_str.unwrap()(self.0.get(), buf.as_mut_ptr().cast(), &mut len);
        if ptr.is_null() {
            return CliValue::String(None);
        }
        if ptr.cast() == buf.as_ptr() {
            buf.truncate(len.try_into().unwrap());
            let res = String::from_utf8(buf).expect("got a buffer that isn't utf8");
            CliValue::String(Some(res))
        } else {
            // figure out where the buffer lives otherwise
            panic!("buffer too long: needs length {len}");
        }
    }
}
