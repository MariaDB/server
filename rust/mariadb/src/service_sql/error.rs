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
        match value {
            _ => Self::Unspecified,
        }
    }
}

impl Display for ClientError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ClientError::ConnectError(n, e) => write!(f, "connection failed with {n}: '{e}'"),
            ClientError::QueryError(n, e) => write!(f, "query failed with {n}: '{e}'"),
            ClientError::FetchError(n, e) => write!(f, "fetch failed with {n}: '{e}'"),
            ClientError::Unspecified => write!(f, "unspecified error"),
        }
    }
}
