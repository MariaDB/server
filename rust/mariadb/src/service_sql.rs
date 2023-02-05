//! Safe API for `include/mysql/service_sql.h`

//!
//! FIXME: I think we need to use a different `GLOBAL_SQL_SERVICE` if statically
//! linked, but not yet sure where this is

use std::cell::UnsafeCell;
use std::ffi::CString;
use std::marker::PhantomData;
use std::ptr::{self, NonNull};

mod error;
mod raw;
use bindings::sql_service as GLOBAL_SQL_SERVICE;
use raw::RawConnection;

pub use self::error::ClientError;
use self::raw::{ClientResult, FetchedRow, RState, RawResult};
pub use self::raw::{Fetch, Store};
use crate::bindings;
use crate::plugin::wrapper::UnsafeSyncCell;

/// A connection to a local or remote SQL server
pub struct MySqlConn(RawConnection);

impl MySqlConn {
    /// Connect to the local server
    ///
    /// # Errors
    ///
    /// Error if the client could not connect
    #[inline]
    pub fn connect_local() -> ClientResult<Self> {
        let mut conn = RawConnection::new();
        conn.connect_local()?;
        Ok(Self(conn))
    }

    /// Run a query and discard the results
    ///
    /// # Errors
    ///
    /// Error if the query could not be completed
    #[inline]
    pub fn execute(&mut self, q: &str) -> ClientResult<()> {
        self.0.query(q)?;
        Ok(())
    }

    /// Run a query for lazily loaded results
    ///
    /// # Errors
    ///
    /// Error if the row could not be fetched
    #[inline]
    pub fn query<'a>(&'a mut self, q: &str) -> ClientResult<FetchedRows<'a>> {
        self.0.query(q)?;
        let res = self.0.prep_fetch_result()?;
        // let cols =
        Ok(FetchedRows(res))
    }
}

/// Representation of returned rows from a query
pub struct FetchedRows<'a>(RawResult<'a, Fetch>);

impl<'a> FetchedRows<'a> {
    #[inline]
    pub fn next_row(&mut self) -> Option<FetchedRow> {
        self.0.fetch_next_row()
    }
}

impl Drop for FetchedRows<'_> {
    /// Consume the rest of the rows
    #[inline]
    fn drop(&mut self) {
        while self.next_row().is_some() {}
    }
}
