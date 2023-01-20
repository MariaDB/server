//! Safe API for include/mysql/service_sql.h

//!
//! FIXME: I think we need to use a different GLOBAL_SQL_SERVICE if statically
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
pub use self::raw::{Fetch, Store};
use self::raw::{RState, RawResult, RawRow, RowsIter};
use crate::bindings;
use crate::plugin::wrapper::UnsafeSyncCell;

pub struct MySqlConn(RawConnection);

/// Need to noodle on this for a bit. I think it makes sense that we have a `&`
/// reference to the creating `MySqlConn`, since that means the connection won't
/// be dropped too early, and can't be changed.
pub struct QueryResult<'a, S: RState> {
    res: RawResult<S>,
    conn: &'a MySqlConn,
}

pub struct Row<'a, S: RState> {
    inner: RawRow,
    res: &'a RawResult<S>,
}

impl MySqlConn {
    /// Connect to the local server
    pub fn connect_local() -> Result<Self, ClientError> {
        let conn = RawConnection::new();
        conn.connect_local()?;
        Ok(Self(conn))
    }

    /// Run a query and discard the results
    pub fn execute<'a>(&'a self, q: &str) -> Result<(), ClientError> {
        self.0.real_query(q)?;
        Ok(())
    }
    /// Run a query for lazily loaded results
    pub fn query<'a>(&'a self, q: &str) -> Result<QueryResult<'a, Fetch>, ClientError> {
        self.0.real_query(q)?;
        let res = self.0.fetch_result()?;
        Ok(QueryResult { res, conn: &self })
    }
}

// impl QueryResult<Fetch> {
//     pub fn iter_rows(self) -> RowsIter {
//         RowsIter
//     }
// }
