//! Safe API for a `MySql` connection
//!
//! `RawConnection` comes almost directly from the `diesel` client crate, since
//! they have that all figured out pretty well. Reference:
//! <https://github.com/diesel-rs/diesel/blob/88129db2fbed49d3ecd41bafff2a5932f1621c2c/diesel/src/mysql/connection/raw.rs>

use std::cell::UnsafeCell;
use std::ffi::{c_void, CStr, CString};
use std::marker::PhantomData;
use std::ptr::NonNull;
use std::sync::Once;
use std::{mem, ptr, slice, str};

use bindings::{sql_service as GLOBAL_SQL_SERVICE, sql_service_st};

use super::error::ClientError;
use crate::{bindings, Value};

/// Get a function from our global SQL service
macro_rules! global_func {
    ($fname:ident) => {
        unsafe { (*GLOBAL_SQL_SERVICE).$fname.unwrap() }
    };
}

pub struct Fetch;
pub struct Store;
pub trait RState {}

impl RState for Fetch {}
impl RState for Store {}

/// Type wrapper for `Result` with a `ClientError` error variant
pub type ClientResult<T> = Result<T, ClientError>;

/// A connection to a remote or local server
pub struct RawConnection(NonNull<bindings::MYSQL>);

/// Options for connecting to a remote SQL server
pub struct ConnOpts {
    host: Option<CString>,
    database: Option<CString>,
    user: Option<CString>,
    password: Option<CString>,
    port: Option<u16>,
    unix_socket: Option<CString>,
    flags: u32,
}

impl RawConnection {
    /// Create a new connection
    pub(super) fn new() -> Self {
        fn_thread_unsafe_lib_init();
        // Attempt to connect, fail if nonzero (unexpected)
        let p_conn = unsafe { global_func!(mysql_init_func)(ptr::null_mut()) };
        let p_conn = NonNull::new(p_conn).expect("OOM: connection allocation failed");
        let result = Self(p_conn);

        // Validate we are using an expected charset
        let charset = unsafe {
            global_func!(mysql_options_func)(
                result.0.as_ptr(),
                bindings::mysql_option::MYSQL_SET_CHARSET_NAME,
                b"utf8mb4\0".as_ptr().cast(),
            )
        };
        assert_eq!(
            0, charset,
            "MYSQL_SET_CHARSET_NAME value of {charset} not recognized"
        );

        result
    }

    /// Connect to the local SQL server
    pub(super) fn connect_local(&mut self) -> ClientResult<()> {
        let res = unsafe { global_func!(mysql_real_connect_local_func)(self.0.as_ptr()) };
        self.check_for_errors(ClientError::ConnectError)?;
        if res.is_null() {
            Ok(())
        } else {
            Err(ClientError::ConnectError(
                0,
                "unspecified connect error".to_owned(),
            ))
        }
    }

    /// Connect to a remote server
    pub fn connect(&mut self, conn_opts: &ConnOpts) -> ClientResult<()> {
        let host = conn_opts.host.as_ref();
        let user = conn_opts.user.as_ref();
        let pw = conn_opts.password.as_ref();
        let db = conn_opts.database.as_ref();
        let port = conn_opts.port;
        let sock = conn_opts.unix_socket.as_ref();

        // TODO: see CLIENT_X flags in mariadb_com.h
        let res = unsafe {
            // Make sure you don't use the fake one!
            global_func!(mysql_real_connect_func)(
                self.0.as_ptr(),
                host.map_or(ptr::null(), |c| c.as_ptr()),
                user.map_or(ptr::null(), |c| c.as_ptr()),
                pw.map_or(ptr::null(), |c| c.as_ptr()),
                db.map_or(ptr::null(), |c| c.as_ptr()),
                port.map_or(0, std::convert::Into::into),
                sock.map_or(ptr::null(), |c| c.as_ptr()),
                conn_opts.flags.into(),
            )
        };

        self.check_for_errors(ClientError::ConnectError)?;

        if res.is_null() {
            Ok(())
        } else {
            Err(ClientError::QueryError(
                0,
                "unspecified query error".to_owned(),
            ))
        }
    }

    /// Execute a query
    pub fn query(&mut self, q: &str) -> ClientResult<()> {
        unsafe {
            let p_self: *const Self = self;
            // mysql_real_query in mariadb_lib.c. Real just means use buffers
            // instead of c strings
            let res = global_func!(mysql_real_query_func)(
                p_self.cast_mut().cast(),
                q.as_ptr().cast(),
                q.len().try_into().unwrap(),
            );
            self.check_for_errors(ClientError::QueryError)?;

            if res == 0 {
                Ok(())
            } else {
                Err(ClientError::QueryError(
                    0,
                    "unspecified query error".to_owned(),
                ))
            }
        }
    }

    /// Prepare the result for iteration, but do not store
    pub fn prep_fetch_result(&mut self) -> ClientResult<RawResult<Fetch>> {
        let res = unsafe { bindings::mysql_use_result(self.0.as_ptr()) };
        self.check_for_errors(ClientError::QueryError)?;

        match NonNull::new(res) {
            Some(ptr) => unsafe {
                let field_count = get_field_count(self, ptr.as_ptr())?;
                let field_ptr = bindings::mysql_fetch_fields(ptr.as_ptr());
                let fields = slice::from_raw_parts(field_ptr, field_count as usize);
                Ok(RawResult {
                    inner: ptr,
                    fields: *fields.as_ptr().cast(),
                    _marker: PhantomData,
                })
            },
            None => Err(ClientError::FetchError(
                0,
                "unspecified fetch error".to_owned(),
            )),
        }
    }

    /// Get the last error message if available and if so, apply it to function `f`
    ///
    /// `f` is usually a variant of `ClientError::SomeError`, since those are functions
    pub fn check_for_errors<F>(&mut self, f: F) -> ClientResult<()>
    where
        F: FnOnce(u32, String) -> ClientError,
    {
        unsafe {
            let cs = CStr::from_ptr(global_func!(mysql_error_func)(self.0.as_ptr()));
            let emsg = cs.to_string_lossy();
            let errno = global_func!(mysql_errno_func)(self.0.as_ptr());

            if emsg.is_empty() && errno == 0 {
                Ok(())
            } else {
                Err(f(errno, emsg.into_owned()))
            }
        }
    }
}

impl Drop for RawConnection {
    /// Close the connection
    fn drop(&mut self) {
        unsafe { global_func!(mysql_close_func)(self.0.as_ptr()) };
    }
}

/// Thin wrapper over a result
pub struct RawResult<'a, S: RState> {
    inner: NonNull<bindings::MYSQL_RES>,
    fields: &'a [Field],
    _marker: PhantomData<S>,
}

impl<'a> RawResult<'a, Fetch> {
    pub fn fetch_next_row(&mut self) -> Option<FetchedRow> {
        let rptr = unsafe { global_func!(mysql_fetch_row_func)(self.inner.as_ptr()) };

        if rptr.is_null() {
            None
        } else {
            Some(FetchedRow {
                inner: rptr,
                fields: self.fields,
            })
        }
    }
}

impl<'a, S: RState> Drop for RawResult<'a, S> {
    /// Free the result
    fn drop(&mut self) {
        unsafe { global_func!(mysql_free_result_func)(self.inner.as_ptr()) };
    }
}

/// Representation of a single row, as part of a SQL query
pub struct FetchedRow<'a> {
    // *mut *mut c_char
    inner: bindings::MYSQL_ROW,
    fields: &'a [Field],
}

impl FetchedRow<'_> {
    /// Get the field of a given index
    pub fn field_value(&self, index: usize) -> Value {
        let field = &self.fields[index];
        assert!(index < self.fields.len()); // extra sanity check
        unsafe {
            let ptr = *self.inner.add(index);
            Value::from_ptr(field.ftype(), ptr.cast(), field.length())
        }
    }

    pub const fn field_info(&self, index: usize) -> &Field {
        &self.fields[index]
    }

    /// Get the total number of fields
    pub const fn field_count(&self) -> usize {
        self.fields.len()
    }
}

/// Transparant wrapper around a `MYSQL_FIELD`
#[repr(transparent)]
pub struct Field(UnsafeCell<bindings::MYSQL_FIELD>);

impl Field {
    pub fn length(&self) -> usize {
        unsafe { (*self.0.get()).length.try_into().unwrap() }
    }

    pub fn max_length(&self) -> usize {
        unsafe { (*self.0.get()).max_length.try_into().unwrap() }
    }

    pub fn name(&self) -> &str {
        unsafe {
            let inner = &*self.0.get();
            let name_slice = slice::from_raw_parts(inner.name.cast(), inner.name_length as usize);
            str::from_utf8(name_slice).expect("unexpected: non-utf8 identifier")
        }
    }

    fn ftype(&self) -> bindings::enum_field_types {
        unsafe { (*self.0.get()).type_ }
    }
}

unsafe fn get_field_count(
    conn: &mut RawConnection,
    q_result: *const bindings::MYSQL_RES,
) -> ClientResult<u32> {
    let res = unsafe { global_func!(mysql_num_fields_func)(q_result.cast_mut()) };
    conn.check_for_errors(ClientError::QueryError)?;
    Ok(res)
}

fn fn_thread_unsafe_lib_init() {
    /// <https://dev.mysql.com/doc/refman/5.7/en/mysql-init.html>
    static MYSQL_THREAD_UNSAFE_INIT: Once = Once::new();

    MYSQL_THREAD_UNSAFE_INIT.call_once(|| {
        // mysql_library_init is defined by `#define mysql_library_init mysql_server_init`
        // which isn't picked up by bindgen
        let ret = unsafe { bindings::mysql_server_init(0, ptr::null_mut(), ptr::null_mut()) };
        assert_eq!(
            ret, 0,
            "Unable to perform MySQL global initialization. Return code: {ret}"
        );
    });
}
