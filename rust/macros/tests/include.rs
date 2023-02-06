/* Simple setup for dummy proc macro testing */

use mariadb::plugin::encryption::*;
use mariadb::plugin::prelude::*;
pub use mariadb_macros::register_plugin;

struct TestPlugin;

impl KeyManager for TestPlugin {
    fn get_latest_key_version(_key_id: u32) -> Result<u32, KeyError> {
        todo!()
    }
    fn get_key(_key_id: u32, _key_version: u32, _dst: &mut [u8]) -> Result<(), KeyError> {
        todo!()
    }
    fn key_length(_key_id: u32, _key_version: u32) -> Result<usize, KeyError> {
        todo!()
    }
}

impl Init for TestPlugin {
    fn init() -> Result<(), InitError> {
        todo!()
    }

    fn deinit() -> Result<(), InitError> {
        todo!()
    }
}
