include!("../include.rs");

register_plugin! {
    TestPlugin,
    ptype: PluginType::MariaEncryption,
    name: "debug_key_management",
    author: "Trevor Gross",
    description: "Debug key management plugin",
    license: License::Gpl,
    maturity: Maturity::Experimental,
    version: "0.1",
    init: TestPlugin,
    encryption: false,
    variables: [
        SysVar {
            ident: _SYSVAR_STR,
            vtype: SysVarString,
            name: "test_sysvar",
            description: "this is a description",
            options: [PluginVarInfo::ReadOnly, PluginVarInfo::NoCmdOpt],
            default: "something"
        }
    ]
}

fn main() {
    // use std::ffi::CStr;
    use std::ptr;

    use mariadb::bindings::{st_maria_plugin, st_mysql_sys_var, sysvar_common_t, sysvar_str_t};
    use mariadb::internals::UnsafeSyncCell;

    // verify correct symbols are created
    let _: i32 = _maria_plugin_interface_version_;
    let _: i32 = _maria_sizeof_struct_st_plugin_;
    let plugin_def: &st_maria_plugin = unsafe { &*(_maria_plugin_declarations_[0]).get() };

    let sysv_ptr: *mut *mut st_mysql_sys_var = plugin_def.system_vars;
    let sysvar_st: *const sysvar_str_t = _st_sysvar_test_sysvar.get();
    let sysvar_arr: &[UnsafeSyncCell<*mut sysvar_common_t>] = &_plugin_debug_key_management_sysvars;
    let idx_0: *mut sysvar_common_t = unsafe { *sysvar_arr[0].get() };
    let idx_1: *mut sysvar_common_t = unsafe { *sysvar_arr[1].get() };
    assert_eq!(idx_0, sysvar_st.cast_mut().cast());
    assert_eq!(idx_1, ptr::null_mut());
    assert_eq!(sysv_ptr, sysvar_arr.as_ptr().cast_mut().cast());
}
