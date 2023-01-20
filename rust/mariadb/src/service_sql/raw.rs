//! Safe API for a MySql connection
//!
//! RawConnection comes almost directly from the `diesel` client crate, since
//! they have that all figured out pretty well. Reference:
//! https://github.com/diesel-rs/diesel/blob/88129db2fbed49d3ecd41bafff2a5932f1621c2c/diesel/src/mysql/connection/raw.rs#L223

use std::ffi::{CStr, CString};
use std::marker::PhantomData;
use std::ptr;
use std::ptr::NonNull;
use std::sync::Once;

use bindings::sql_service as GLOBAL_SQL_SERVICE;

use super::error::ClientError;
use crate::bindings;

pub struct Fetch;
pub struct Store;
pub trait RState {}

impl RState for Fetch {}
impl RState for Store {}

pub struct RawConnection(NonNull<bindings::MYSQL>);
pub struct RawResult<S: RState>(NonNull<bindings::MYSQL_RES>, PhantomData<S>);

pub struct Row<'a, S: RState> {
    inner: bindings::MYSQL_ROW,
    res: &'a RawResult<S>,
}

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
    pub(super) fn new() -> Self {
        fn_thread_unsafe_lib_init();
        let p_conn = unsafe { (*GLOBAL_SQL_SERVICE).mysql_init_func.unwrap()(ptr::null_mut()) };
        let p_conn = NonNull::new(p_conn).expect("OOM: connection allocation failed");
        let result = RawConnection(p_conn);

        // let charset_result = unsafe {
        //     bindings::mysql_options(
        //         result.0.as_ptr(),
        //         bindings::mysql_option::MYSQL_SET_CHARSET_NAME,
        //         b"utf8mb4\0".as_ptr() as *const libc::c_void,
        //     )
        // };
        // assert_eq!(
        //     0, charset_result,
        //     "MYSQL_SET_CHARSET_NAME was not \
        //      recognized as an option by MySQL. This should never \
        //      happen."
        // );

        result
    }

    pub(super) fn connect_local(&self) -> Result<(), ClientError> {
        let res = unsafe {
            (*GLOBAL_SQL_SERVICE).mysql_real_connect_local_func.unwrap()(self.0.as_ptr())
        };
        if res.is_null() {
            Ok(())
        } else {
            Err((res as i32).into())
        }
    }

    pub(super) fn connect(&self, conn_opts: &ConnOpts) -> Result<(), ClientError> {
        let host = conn_opts.host.as_ref();
        let user = conn_opts.user.as_ref();
        let pw = conn_opts.password.as_ref();
        let db = conn_opts.database.as_ref();
        let port = conn_opts.port;
        let sock = conn_opts.unix_socket.as_ref();

        // TODO: see CLIENT_X flags in mariadb_com.h
        let res = unsafe {
            // Make sure you don't use the fake one!
            (*GLOBAL_SQL_SERVICE).mysql_real_connect_func.unwrap()(
                self.0.as_ptr(),
                host.map(|c| c.as_ptr()).unwrap_or(ptr::null()),
                user.map(|c| c.as_ptr()).unwrap_or(ptr::null()),
                pw.map(|c| c.as_ptr()).unwrap_or(ptr::null()),
                db.map(|c| c.as_ptr()).unwrap_or(ptr::null()),
                port.map(|p| p.into()).unwrap_or(0),
                sock.map(|c| c.as_ptr()).unwrap_or(ptr::null()),
                conn_opts.flags.into(),
            )
        };

        if res.is_null() {
            Ok(())
        } else {
            Err((res as i32).into())
        }

        // let last_error_message = self.last_error_message();
        // if last_error_message.is_empty() {
        //     Ok(())
        // } else {
        //     Err(ConnectionError::BadConnection(last_error_message))
        // }
    }

    pub fn real_query(&self, q: &str) -> Result<(), ClientError> {
        unsafe {
            let p_self: *const Self = self;
            // mysql_real_query in mariadb_lib.c
            let res = (*GLOBAL_SQL_SERVICE).mysql_real_query_func.unwrap()(
                p_self.cast_mut().cast(),
                q.as_ptr().cast(),
                q.len().try_into().unwrap(),
            );

            if res == 0 {
                Ok(())
            } else {
                Err(res.into())
            }
        }
    }

    pub fn fetch_result(&self) -> Result<RawResult<Fetch>, ClientError> {
        let res = unsafe { bindings::mysql_use_result(self.0.as_ptr()) };

        match NonNull::new(res) {
            Some(ptr) => Ok(RawResult(ptr, PhantomData)),
            None => Err((res as i32).into()),
        }
    }
}

impl RawResult<Fetch> {
    pub fn fetch_row(&mut self) -> Option<Row<Fetch>> {
        let rptr = unsafe {bindings::server_mysql_fetch_row(self.0.as_ptr())};

        if rptr.is_null() {
            None
        } else {
            Some(Row { inner: rptr, res: self })
        }
    }
}

impl Drop for RawConnection {
    fn drop(&mut self) {
        unsafe { (*GLOBAL_SQL_SERVICE).mysql_close_func.unwrap()(self.0.as_ptr()) };
    }
}

impl<S: RState> Drop for RawResult<S> {
    fn drop(&mut self) {
        unsafe { (*GLOBAL_SQL_SERVICE).mysql_free_result_func.unwrap()(self.0.as_ptr()) };
    }
}

/// <https://dev.mysql.com/doc/refman/5.7/en/mysql-init.html>
static MYSQL_THREAD_UNSAFE_INIT: Once = Once::new();

fn fn_thread_unsafe_lib_init() {
    MYSQL_THREAD_UNSAFE_INIT.call_once(|| {
        // mysql_library_init is defined by `#define mysql_library_init mysql_server_init`
        // which isn't picked up by bindgen
        let ret = unsafe { bindings::mysql_server_init(0, ptr::null_mut(), ptr::null_mut()) };
        if ret != 0 {
            panic!("Unable to perform MySQL global initialization. Return code: {ret}");
        }
    })
}
