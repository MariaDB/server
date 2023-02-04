use std::ffi::c_void;
use std::slice;

use crate::bindings;

/// A SQL type and value
#[non_exhaustive]
pub enum Value<'a> {
    Decimal(&'a [u8]),
    Tiny(i8),
    Short(i16),
    Long(i64),
    Float(f32),
    Double(f64),
    Null,
    Time,      // todo
    TimeStamp, // todo
    Date,      // todo
    DateTime,  // todo
    Year,      // todo
    Varchar(&'a [u8]),
    Json(&'a [u8]),
}

impl<'a> Value<'a> {
    /// Supply a
    pub(crate) unsafe fn from_ptr(
        ty: bindings::enum_field_types,
        ptr: *const c_void,
        len: usize,
    ) -> Self {
        // helper function to make a slice
        let buf_callback = || slice::from_raw_parts(ptr.cast(), len);

        match ty {
            bindings::enum_field_types::MYSQL_TYPE_DECIMAL => Self::Decimal(buf_callback()),
            bindings::enum_field_types::MYSQL_TYPE_TINY => Self::Tiny(*ptr.cast()),
            bindings::enum_field_types::MYSQL_TYPE_SHORT => Self::Short(*ptr.cast()),
            bindings::enum_field_types::MYSQL_TYPE_LONG => Self::Long(*ptr.cast()),
            bindings::enum_field_types::MYSQL_TYPE_FLOAT => Self::Float(*ptr.cast()),
            bindings::enum_field_types::MYSQL_TYPE_DOUBLE => Self::Double(*ptr.cast()),
            bindings::enum_field_types::MYSQL_TYPE_NULL => Self::Null,
            bindings::enum_field_types::MYSQL_TYPE_TIMESTAMP => todo!(),
            bindings::enum_field_types::MYSQL_TYPE_LONGLONG => todo!(),
            bindings::enum_field_types::MYSQL_TYPE_INT24 => todo!(),
            bindings::enum_field_types::MYSQL_TYPE_DATE => todo!(),
            bindings::enum_field_types::MYSQL_TYPE_TIME => todo!(),
            bindings::enum_field_types::MYSQL_TYPE_DATETIME => todo!(),
            bindings::enum_field_types::MYSQL_TYPE_YEAR => todo!(),
            bindings::enum_field_types::MYSQL_TYPE_NEWDATE => todo!(),
            bindings::enum_field_types::MYSQL_TYPE_VARCHAR => todo!(),
            bindings::enum_field_types::MYSQL_TYPE_BIT => todo!(),
            bindings::enum_field_types::MYSQL_TYPE_TIMESTAMP2 => todo!(),
            bindings::enum_field_types::MYSQL_TYPE_DATETIME2 => todo!(),
            bindings::enum_field_types::MYSQL_TYPE_TIME2 => todo!(),
            bindings::enum_field_types::MYSQL_TYPE_BLOB_COMPRESSED => todo!(),
            bindings::enum_field_types::MYSQL_TYPE_VARCHAR_COMPRESSED => todo!(),
            bindings::enum_field_types::MYSQL_TYPE_NEWDECIMAL => todo!(),
            bindings::enum_field_types::MYSQL_TYPE_ENUM => todo!(),
            bindings::enum_field_types::MYSQL_TYPE_SET => todo!(),
            bindings::enum_field_types::MYSQL_TYPE_TINY_BLOB => todo!(),
            bindings::enum_field_types::MYSQL_TYPE_MEDIUM_BLOB => todo!(),
            bindings::enum_field_types::MYSQL_TYPE_LONG_BLOB => todo!(),
            bindings::enum_field_types::MYSQL_TYPE_BLOB => todo!(),
            bindings::enum_field_types::MYSQL_TYPE_VAR_STRING => todo!(),
            bindings::enum_field_types::MYSQL_TYPE_STRING => todo!(),
            bindings::enum_field_types::MYSQL_TYPE_GEOMETRY => todo!(),
            _ => todo!(),
        }
    }
}
