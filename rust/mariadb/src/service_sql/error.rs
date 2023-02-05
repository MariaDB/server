use std::fmt::Display;

use crate::bindings;

#[non_exhaustive]
pub enum ClientError {
    // CommandsOutOfSync = bindings::CR_COMMANDS_OUT_OF_SYNC
    /// Error connecting
    ConnectError(u32, String),
    QueryError(u32, String),
    FetchError(u32, String),
    Unspecified,
}

impl From<i32> for ClientError {
    fn from(value: i32) -> Self {
        Self::Unspecified
    }
}

impl Display for ClientError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::ConnectError(n, e) => write!(f, "connection failed with {n}: '{e}'"),
            Self::QueryError(n, e) => write!(f, "query failed with {n}: '{e}'"),
            Self::FetchError(n, e) => write!(f, "fetch failed with {n}: '{e}'"),
            Self::Unspecified => write!(f, "unspecified error"),
        }
    }
}
