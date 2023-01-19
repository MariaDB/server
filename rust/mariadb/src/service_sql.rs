//! Safe API for include/mysql/service_sql.h
//!
//! FIXME: I think we need to use a different GLOBAL_SQL_SERVICE if statically
//! linked, but not yet sure where this is

use std::cell::UnsafeCell;
use std::ffi::CString;
use std::marker::PhantomData;
use std::ptr;

use bindings::{sql_service as GLOBAL_SQL_SERVICE, sql_service_st, MYSQL};

use crate::bindings;
use crate::plugin::wrapper::UnsafeSyncCell;

pub trait MySqlState {}
pub struct Connected;
pub struct Disconnected;
impl MySqlState for Connected {}
impl MySqlState for Disconnected {}

pub struct MySql<S: MySqlState>(UnsafeCell<MYSQL>, PhantomData<S>);

impl MySql<Disconnected> {
    fn try_new<'a>() -> Option<&'a Self> {
        unsafe {
            // SAFETY: GLOBAL_SQL_SERVICE is valid (linked?)
            let p = (*GLOBAL_SQL_SERVICE).mysql_init_func.unwrap()(ptr::null_mut());
            // SAFETY: We are casting to an option, so nullptr is handled that
            // way. Otherwise, return of `mysql_init_func` is valid.
            *p.cast()
        }
    }

    /// Attempt to connect
    fn try_connect_local<'a>(&'a mut self) -> Option<&'a MySql<Connected>> {
        unsafe {
            let p_self: *mut Self = self;
            let p = (*GLOBAL_SQL_SERVICE).mysql_real_connect_local_func.unwrap()(p_self.cast());
            *p.cast()
        }
    }

    // TODO: see CLIENT_X flags in mariadb_com.h
    fn try_connect<'a>(
        &'a mut self,
        host: &str,
        database: &str,
        user: &str,
        password: &str,
        port: u16,
        unix_socket: &str,
        client_flag: u32,
    ) -> Option<&'a MySql<Connected>> {
        const EMSG: &str = "provided string contains null";
        let p_self: *mut Self = self;
        let host_cs = CString::new(host).expect(EMSG);
        let db_cs = CString::new(database).expect(EMSG);
        let user_cs = CString::new(user).expect(EMSG);
        let pw_cs = CString::new(password).expect(EMSG);
        let sock_cs = CString::new(unix_socket).expect(EMSG);

        unsafe {
            // see mysql_real_connect in mariadb_lib.c
            let p = (*GLOBAL_SQL_SERVICE).mysql_real_connect_func.unwrap()(
                p_self.cast(),
                host_cs.as_ptr(),
                user_cs.as_ptr(),
                pw_cs.as_ptr(),
                db_cs.as_ptr(),
                port.into(),
                sock_cs.as_ptr(),
                client_flag.into(),
            );
            *p.cast()
        }
    }
}

impl MySql<Connected> {
    fn real_query() {}
}
