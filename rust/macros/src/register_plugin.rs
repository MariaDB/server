//!

#![allow(unused)]

use proc_macro::TokenStream;
use proc_macro2::{Literal, Span, TokenTree};
use quote::quote;
use syn::parse::{Parse, ParseStream};
use syn::punctuated::Punctuated;
use syn::token::Group;
use syn::{
    parse_macro_input, parse_quote, Attribute, DeriveInput, Error, Expr, ExprLit, FieldValue,
    Ident, ImplItem, ImplItemType, Item, ItemImpl, Lit, LitStr, Path, PathSegment, Token, Type,
    TypePath, TypeReference,
};

/// Entrypoint for this proc macro
pub fn entry(tokens: TokenStream) -> TokenStream {
    let tokens_pm2: proc_macro2::TokenStream = tokens.clone().into();
    let input = parse_macro_input!(tokens as PluginInfo);
    let plugindef = input.to_encryption_struct();
    match plugindef {
        Ok(ts) => ts.into_output(),
        Err(e) => e.into_compile_error().into(),
    }
}

/// A representation of the contents of a registration macro. This macro will be
/// the same for
#[derive(Clone, Debug)]
struct PluginInfo {
    /// The main type that has required methods implemented on it
    main_ty: Ident,
    span: Span,
    ptype: Option<Expr>,
    name: Option<Expr>,
    author: Option<Expr>,
    description: Option<Expr>,
    license: Option<Expr>,
    maturity: Option<Expr>,
    version: Option<Expr>,
    init: Option<Expr>,
    encryption: Option<Expr>,
}

impl Parse for PluginInfo {
    fn parse(input: ParseStream) -> syn::Result<Self> {
        let main_ty = input.parse()?;
        // FIXME: span is only the beginning
        let span = input.span();
        let mut ret = Self::new(main_ty, span);
        let _: Token![,] = input.parse()?;

        let fields = Punctuated::<FieldValue, Token![,]>::parse_terminated(input)?;
        let mut field_order: Vec<String> = Vec::new();
        for field in fields.clone() {
            let syn::Member::Named(name) = &field.member else {
                return Err(Error::new_spanned(field, "missing field name"));
            };

            let name_str = name.to_string();
            let expr = field.expr;

            match name_str.as_str() {
                "ptype" => ret.ptype = Some(expr),
                "name" => ret.name = Some(expr),
                "author" => ret.author = Some(expr),
                "description" => ret.description = Some(expr),
                "license" => ret.license = Some(expr),
                "maturity" => ret.maturity = Some(expr),
                "version" => ret.version = Some(expr),
                "init" => ret.init = Some(expr),
                "encryption" => ret.encryption = Some(expr),
                _ => {
                    return Err(Error::new_spanned(
                        name,
                        format!("unexpected field '{name_str}'"),
                    ))
                }
            }
            field_order.push(name_str);
        }

        if let Err(msg) = verify_field_order(field_order.as_slice()) {
            return Err(Error::new_spanned(fields, msg));
        }
        Ok(ret)
    }
}

impl PluginInfo {
    fn new(main_ty: Ident, span: Span) -> Self {
        Self {
            main_ty,
            span,
            ptype: None,
            name: None,
            author: None,
            description: None,
            license: None,
            maturity: None,
            version: None,
            init: None,
            encryption: None,
        }
    }

    /// Ensure we have the fields that are required for all plugin types
    fn validate_correct_fields(
        &self,
        required: &[&str],
        optional: &[&str],
        ptype: &str,
    ) -> syn::Result<()> {
        // These are all required for all plugin types
        let name_map = [
            (&self.ptype, "ptype"),
            (&self.name, "name"),
            (&self.author, "author"),
            (&self.description, "description"),
            (&self.license, "license"),
            (&self.maturity, "maturity"),
            (&self.version, "version"),
            (&self.init, "init"),
            (&self.encryption, "encryption"),
        ];

        let mut req = vec![
            "ptype",
            "name",
            "author",
            "description",
            "license",
            "maturity",
            "version",
        ];
        req.extend_from_slice(required);

        for req_field in req.iter() {
            let (field_val, fname) = name_map.iter().find(|f| f.1 == *req_field).unwrap();

            if field_val.is_none() {
                let msg = format!("field '{fname}' is expected for plugins of type {ptype}, but not provided\n(in macro 'register_plugin')");
                return Err(Error::new(self.span, msg));
            }
        }

        for (field, fname) in name_map {
            if field.is_some() && !req.contains(&fname) && !optional.contains(&fname) {
                let msg = format!("field '{fname}' is not expected for plugins of type {ptype}\n(in macro 'register_plugin')");
                return Err(Error::new_spanned(field.as_ref().unwrap(), msg));
            }
        }

        Ok(())
    }

    /// Ensure we have the fields required for an encryption plugin
    fn validate_as_encryption(&self) -> syn::Result<()> {
        let optional = ["init"];
        let required = ["encryption"];
        self.validate_correct_fields(&required, &optional, "encryption")?;
        Ok(())
    }

    /// Turn `self` into a tokenstream of a single `st_maria_plugin` for an
    /// encryption struct. Uses `idx` to mangle the name and avoid conflicts
    fn to_encryption_struct(self) -> syn::Result<PluginDef> {
        self.validate_as_encryption()?;

        let type_ = &self.main_ty;
        let name = expect_litstr(&self.name)?;
        let plugin_st_name = Ident::new(&format!("_ST_PLUGIN_{}", name.value()), Span::call_site());

        let ty_as_wkeymgt =
            quote! { <#type_ as ::mariadb::plugin::encryption_wrapper::WrapKeyMgr> };
        let ty_as_wenc =
            quote! { <#type_ as ::mariadb::plugin::encryption_wrapper::WrapEncryption> };
        let interface_version = quote! { ::mariadb::bindings::MariaDB_ENCRYPTION_INTERFACE_VERSION as ::std::ffi::c_int };
        let get_key_vers = quote! { Some(#ty_as_wkeymgt::wrap_get_latest_key_version) };
        let get_key = quote! { Some(#ty_as_wkeymgt::wrap_get_key) };

        let (crypt_size, crypt_init, crypt_update, crypt_finish, crypt_len);

        if expect_bool(&self.encryption)? {
            // Use encryption if given
            crypt_size = quote! { Some(#ty_as_wenc::wrap_crypt_ctx_size) };
            crypt_init = quote! { Some(#ty_as_wenc::wrap_crypt_ctx_init) };
            crypt_update = quote! { Some(#ty_as_wenc::wrap_crypt_ctx_update) };
            crypt_finish = quote! { Some(#ty_as_wenc::wrap_crypt_ctx_finish) };
            crypt_len = quote! { Some(#ty_as_wenc::wrap_encrypted_length) };
        } else {
            // Default to builtin encryption
            let none = quote! { None };
            (
                crypt_size,
                crypt_init,
                crypt_update,
                crypt_finish,
                crypt_len,
            ) = (none.clone(), none.clone(), none.clone(), none.clone(), none);
        }

        let info_struct = quote! {
            static #plugin_st_name: ::mariadb::plugin::wrapper::UnsafeSyncCell<
                ::mariadb::bindings::st_mariadb_encryption,
            > = unsafe {
                ::mariadb::plugin::wrapper::UnsafeSyncCell::new(
                    ::mariadb::bindings::st_mariadb_encryption {
                        interface_version: #interface_version,
                        get_latest_key_version: #get_key_vers,
                        get_key: #get_key,
                        crypt_ctx_size: #crypt_size,
                        crypt_ctx_init: #crypt_init,
                        crypt_ctx_update: #crypt_update,
                        crypt_ctx_finish: #crypt_finish,
                        encrypted_length: #crypt_len,
                    }
                )
            };
        };

        let version = version_int(&expect_litstr(&self.version)?.value())
            .map_err(|e| Error::new_spanned(&self.version, e))?;
        let author = expect_litstr(&self.author)?;
        let description = expect_litstr(&self.description)?;
        let license = self.license.unwrap();
        let maturity = self.maturity.unwrap();
        let ptype = self.ptype.unwrap();

        let (fn_init, fn_deinit);
        if let Some(init_ty) = self.init {
            let ty_as_init = quote! {<#init_ty as ::mariadb::plugin::wrapper::WrapInit>};
            fn_init = quote! { Some(#ty_as_init::wrap_init) };
            fn_deinit = quote! { Some(#ty_as_init::wrap_deinit) };
        } else {
            let none = quote! { None };
            (fn_init, fn_deinit) = (none.clone(), none);
        }

        let plugin_struct = quote! {
            ::mariadb::bindings::st_maria_plugin {
                type_: #ptype.to_ptype_registration(),
                info: #plugin_st_name.as_ptr().cast_mut().cast(),
                name: ::mariadb::cstr::cstr!(#name).as_ptr(),
                author: ::mariadb::cstr::cstr!(#author).as_ptr(),
                descr: ::mariadb::cstr::cstr!(#description).as_ptr(),
                license: #license.to_license_registration(),
                init: #fn_init,
                deinit: #fn_deinit,
                version: #version as ::std::ffi::c_uint,
                status_vars: ::std::ptr::null_mut(),
                system_vars: ::std::ptr::null_mut(),
                version_info: ::mariadb::cstr::cstr!("0.1").as_ptr(),
                maturity: #maturity.to_maturity_registration(),
            },
        };

        Ok(PluginDef {
            name: name.value(),
            info_struct,
            plugin_struct,
        })
    }
}

/// Contains a struct definition of type `st_mariadb_encryption` or whatever is
/// applicable, plus the struct of `st_maria_plugin` that references it
struct PluginDef {
    name: String,
    info_struct: proc_macro2::TokenStream,
    plugin_struct: proc_macro2::TokenStream,
}

impl PluginDef {
    fn into_output(self) -> TokenStream {
        let make_ident = |s| Ident::new(s, Span::call_site());
        let make_ident_fmt = |s: String| Ident::new(s.as_str(), Span::call_site());
        // let vers_idt_stc =
        //     make_ident_fmt(format!("builtin_{}_plugin_interface_version", self.name));
        let vers_idt_dyn = make_ident("_maria_plugin_interface_version_");
        // let size_idt_stc = make_ident_fmt(format!("builtin_{}_sizeof_struct_st_plugin", self.name));
        let size_idt_dyn = make_ident("_maria_sizeof_struct_st_plugin_");
        // let decl_idt_stc = make_ident_fmt(format!("builtin_{}_plugin", self.name));
        let decl_idt_dyn = make_ident("_maria_plugin_declarations_");

        let plugin_ty = quote! {::mariadb::bindings::st_maria_plugin};
        let version_val =
            quote! {mariadb::bindings::MARIA_PLUGIN_INTERFACE_VERSION as ::std::ffi::c_int};
        let size_val = quote! {::std::mem::size_of::<#plugin_ty>() as ::std::ffi::c_int};

        let usynccell = quote! {::mariadb::plugin::wrapper::UnsafeSyncCell};
        let null_ps = quote! {::mariadb::plugin::wrapper::new_null_st_maria_plugin()};

        let is = self.info_struct;
        let ps = self.plugin_struct;

        let ret: TokenStream = quote! {
            // Different config based on statically or dynamically lynked

            // #[no_mangle]
            // #[cfg(not(any(crate_type = "dylib", crate_type = "cdylib")))]
            // static #vers_idt_stc: ::std::ffi::c_int = #version_val;

            #[no_mangle]
            // #[cfg(any(crate_type = "dylib", crate_type = "cdylib"))]
            static #vers_idt_dyn: ::std::ffi::c_int = #version_val;

            // #[no_mangle]
            // #[cfg(not(any(crate_type = "dylib", crate_type = "cdylib")))]
            // static #size_idt_stc: ::std::ffi::c_int = #size_val;

            #[no_mangle]
            // #[cfg(any(crate_type = "dylib", crate_type = "cdylib"))]
            static #size_idt_dyn: ::std::ffi::c_int = #size_val;

            // #[no_mangle]
            // #[cfg(not(any(crate_type = "dylib", crate_type = "cdylib")))]
            // static #decl_idt_stc: [#usynccell<#plugin_ty>; 2] = unsafe { [
            //     #usynccell::new(#ps),
            //     #usynccell::new(#null_ps),
            //     ] };

            #[no_mangle]
            // #[cfg(any(crate_type = "dylib", crate_type = "cdylib"))]
            static #decl_idt_dyn: [#usynccell<#plugin_ty>; 2] = unsafe { [
                #usynccell::new(#ps),
                #usynccell::new(#null_ps),
            ] };

            #is
        }
        .into();

        ret
    }
}

/// Verify attribute order
fn verify_field_order(fields: &[String]) -> Result<(), String> {
    let mut expected_order = vec![
        "ptype",
        "name",
        "author",
        "description",
        "license",
        "maturity",
        "version",
        "init",
        "encryption",
    ];

    expected_order.retain(|expected| fields.iter().any(|f| f == expected));

    if expected_order != fields {
        Err(format!(
            "fields not in expected order. reorder as:\n{:?}",
            expected_order
        ))
    } else {
        Ok(())
    }
}

/// Get the field as a boolean
fn expect_bool(field_opt: &Option<Expr>) -> syn::Result<bool> {
    let field = field_opt.as_ref().unwrap();

    if field == &parse_quote! {true} {
        Ok(true)
    } else if field == &parse_quote! {false} {
        Ok(false)
    } else {
        let msg = "unexpected value: only 'true' or 'false' allowed";
        Err(Error::new_spanned(field, msg))
    }
}

/// Expect a literal string, error if that's not the case
fn expect_litstr(field_opt: &Option<Expr>) -> syn::Result<&LitStr> {
    let field = field_opt.as_ref().unwrap();
    let Expr::Lit(lit) = field else { // got non-literal
        let msg = "expected literal expression for this field";
        return Err(Error::new_spanned(field, msg));
    };
    let Lit::Str(litstr) = &lit.lit else { // got literal that wasn't a string
        let msg = "only literal strings are allowed for this field";
        return Err(Error::new_spanned(field, msg));
    };

    Ok(litstr)
}

/// Convert a string like "1.2" to a hex like "0x0102". Error if no decimal, or
/// if either value exceeds a u8.
fn version_int(s: &str) -> Result<u16, String> {
    const USAGE_MSG: &str = r#"expected a two position semvar string, e.g. "1.2""#;
    if s.chars().filter(|x| *x == '.').count() != 1 {
        return Err(USAGE_MSG.to_owned());
    }

    let splt = s.split_once('.').unwrap();
    let fmt_err = |e| format!("{e}\n{USAGE_MSG}");

    let major: u16 = splt.0.parse::<u8>().map_err(fmt_err)?.into();
    let minor: u16 = splt.1.parse::<u8>().map_err(fmt_err)?.into();
    let res: u16 = (major << 8) + minor;

    Ok(res)
}
