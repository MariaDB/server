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
            ident: _SYSVAR_CONST_STR,
            vtype: SysVarConstString,
            name: "test_sysvar",
            description: "this is a description",
            options: [SysVarOpt::ReadOnly, SysVarOpt::NoCmdOpt],
            default: "default value"
        }
    ]
}

fn main() {
    use std::ffi::CStr;
    use std::ptr;

    use mariadb::bindings;
    use mariadb::bindings::{st_maria_plugin, st_mysql_sys_var, sysvar_common_t, sysvar_str_t};
    use mariadb::internals::UnsafeSyncCell;

    // verify correct symbols are created
    let _: i32 = _maria_plugin_interface_version_;
    let _: i32 = _maria_sizeof_struct_st_plugin_;
    let plugin_def: &st_maria_plugin = unsafe { &*(_maria_plugin_declarations_[0]).get() };

    let sysv_ptr: *mut *mut st_mysql_sys_var = plugin_def.system_vars;
    let sysvar_st: *const sysvar_str_t = _sysvar_st_test_sysvar.get();
    let sysvar_arr: &[UnsafeSyncCell<*mut sysvar_common_t>] = &_plugin_debug_key_management_sysvars;
    let idx_0: *mut sysvar_common_t = unsafe { *sysvar_arr[0].get() };
    let idx_1: *mut sysvar_common_t = unsafe { *sysvar_arr[1].get() };
    assert_eq!(idx_0, sysvar_st.cast_mut().cast());
    assert_eq!(idx_1, ptr::null_mut());
    assert_eq!(sysv_ptr, sysvar_arr.as_ptr().cast_mut().cast());

    // try the C way, slow casting steps to avoid errors here
    let sv1_ptr: *mut st_mysql_sys_var = unsafe { *plugin_def.system_vars.add(0) };
    let sv1: &sysvar_str_t = unsafe { &*sv1_ptr.cast() };
    let flags = sv1.flags;
    let sv1_name = unsafe { CStr::from_ptr(sv1.name).to_str().unwrap() };
    let sv1_comment = unsafe { CStr::from_ptr(sv1.comment).to_str().unwrap() };
    let sv1_default = unsafe { CStr::from_ptr(sv1.def_val).to_str().unwrap() };

    let expected_flags = bindings::PLUGIN_VAR_STR
        | ((bindings::PLUGIN_VAR_READONLY | bindings::PLUGIN_VAR_NOCMDOPT)
            & bindings::PLUGIN_VAR_MASK);
    assert_eq!(flags, expected_flags as i32);
    assert_eq!(sv1_name, "test_sysvar");
    assert_eq!(sv1_comment, "this is a description");
    assert_eq!(sv1_default, "default value");
}
