include!("../include.rs");

register_plugin! {
    TestPlugin,
    ptype: PluginType::MariaEncryption,
    name: "test_plugin_name",
    author: "Test Author",
    description: "Debug key management plugin",
    license: License::Gpl,
    maturity: Maturity::Experimental,
    version: "1.2",
    encryption: false,
}

fn main() {
    use std::ffi::CStr;

    use mariadb::bindings::st_maria_plugin;

    // verify correct symbols are created
    let _: i32 = _maria_plugin_interface_version_;
    let _: i32 = _maria_sizeof_struct_st_plugin_;
    let plugin_def: &st_maria_plugin = unsafe { &*(_maria_plugin_declarations_[0]).get() };

    // verify struct has correct fields
    let type_ = plugin_def.type_;
    let name = unsafe { CStr::from_ptr(plugin_def.name).to_str().unwrap() };
    let author = unsafe { CStr::from_ptr(plugin_def.author).to_str().unwrap() };
    let descr = unsafe { CStr::from_ptr(plugin_def.descr).to_str().unwrap() };
    let license = plugin_def.license;
    let init = plugin_def.init;
    let deinit = plugin_def.deinit;
    let version = plugin_def.version;
    let status_vars = plugin_def.status_vars;
    let system_vars = plugin_def.system_vars;
    let version_info = unsafe { CStr::from_ptr(plugin_def.version_info).to_str().unwrap() };
    let maturity = plugin_def.maturity;

    assert_eq!(type_, PluginType::MariaEncryption as i32);
    assert_eq!(name, "test_plugin_name");
    assert_eq!(author, "Test Author");
    assert_eq!(descr, "Debug key management plugin");
    assert_eq!(license, License::Gpl as i32);
    assert!(init.is_some()); // we always have an init function to setup logging
    assert!(deinit.is_none());
    assert_eq!(version, 0x0102);
    assert!(status_vars.is_null());
    assert!(system_vars.is_null());
    assert_eq!(version_info, "1.2");
    assert_eq!(maturity, Maturity::Experimental as u32);
}
