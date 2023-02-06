//! Parse sysvar syntax
//!
//! ```ignore
//! {
//!     ident: SOME_IDENT,
//!     vtype: String,
//!     name: "sql_name",
//!     description: "this is a description",
//!     options: [PluginVarInfo::ReadOnly, PluginVarInfo::NoCmdOpt],
//!     default: "something"
//! }
//! ```
//!
//! or
//!
//! ```ignore
//! {
//!     ident: OTHER_IDENT,
//!     vtype: AtomicI32,
//!     name: "other_sql_name",
//!     description: "this is a description",
//!     options: [PluginVarInfo::ReqCmdArg],
//!     default: 100,
//!     min: 10,
//!     max: 500,
//!     interval: 10
//! }
//! ```

#![allow(unused)]

// use proc_macro::TokenStream;
use proc_macro2::{Literal, Span, TokenStream, TokenTree};
use quote::{quote, ToTokens};
use syn::parse::{Parse, ParseStream};
use syn::punctuated::Punctuated;
use syn::token::Group;
use syn::{
    bracketed, parse_macro_input, parse_quote, Attribute, DeriveInput, Error, Expr, ExprArray,
    ExprLit, ExprStruct, FieldValue, Ident, ImplItem, ImplItemType, Item, ItemImpl, Lit, LitStr,
    Path, PathSegment, Token, Type, TypePath, TypeReference,
};

use crate::fields::sysvar::{
    ALL_FIELDS, NUM_OPT_FIELDS, NUM_REQ_FIELDS, REQ_FIELDS, STR_OPT_FIELDS, STR_REQ_FIELDS,
};
use crate::helpers::expect_litstr;

#[derive(Clone, Copy, Debug)]
enum VarTypeInner {
    SysVar,
    ShowVar,
}

/// Identifiers and their bodies
#[derive(Clone, Debug, Default)]
pub struct Variables {
    pub sys: Vec<TokenStream>,
    pub sys_idents: Vec<Ident>,
    pub show: Vec<TokenStream>,
    pub show_idents: Vec<Ident>,
}

impl Parse for Variables {
    fn parse(input: ParseStream) -> syn::Result<Self> {
        let content;
        let _ = bracketed!(content in input);
        let var_decl = Punctuated::<VariableInfo, Token![,]>::parse_terminated(&content)?;
        let mut ret = Self::default();
        for var in var_decl {
            let ty = var.var_type.unwrap();
            match ty {
                VarTypeInner::SysVar => {
                    let tmp = var.make_usable()?;
                    ret.sys_idents.push(tmp.0);
                    ret.sys.push(tmp.1);
                }
                VarTypeInner::ShowVar => {
                    let tmp = var.make_usable()?;
                    ret.show_idents.push(tmp.0);
                    ret.show.push(tmp.1);
                }
            }
        }
        Ok(ret)
    }
}

#[derive(Clone, Debug, Default)]
struct VariableInfo {
    span: Option<Span>,
    var_type: Option<VarTypeInner>,
    ident: Option<Expr>,
    vtype: Option<Expr>,
    name: Option<Expr>,
    description: Option<Expr>,
    options: Option<Expr>,
    default: Option<Expr>,
    min: Option<Expr>,
    max: Option<Expr>,
    interval: Option<Expr>,
}

impl Parse for VariableInfo {
    fn parse(input: ParseStream) -> syn::Result<Self> {
        let span = input.span();
        let mut ret = Self {
            span: Some(span),
            ..Default::default()
        };

        // parse struct-like syntax
        let st: ExprStruct = input.parse()?;
        // verify the weird possible things of a struct are empty
        if !st.attrs.is_empty() {
            return Err(Error::new_spanned(&st.attrs[0], "no attributes expected"));
        }
        if st.path != parse_quote! { SysVar } {
            return Err(Error::new_spanned(
                &st.path,
                "only path 'SysVar' is allowed",
            ));
        }
        ret.var_type = Some(VarTypeInner::SysVar);
        if st.rest.is_some() {
            return Err(Error::new_spanned(&st.rest, "unexpected 'rest' section"));
        }

        let fields = st.fields;
        let mut field_order: Vec<String> = Vec::new();
        for field in fields.clone() {
            let syn::Member::Named(name) = &field.member else {
                return Err(Error::new_spanned(field, "missing field name"));
            };

            let name_str = name.to_string();
            let expr = field.expr;

            match name_str.as_str() {
                "ident" => ret.ident = Some(expr),
                "vtype" => ret.vtype = Some(expr),
                "name" => ret.name = Some(expr),
                "description" => ret.description = Some(expr),
                "options" => ret.options = Some(expr),
                "default" => ret.default = Some(expr),
                "min" => ret.min = Some(expr),
                "max" => ret.max = Some(expr),
                "interval" => ret.interval = Some(expr),
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

impl VariableInfo {
    fn make_usable(&self) -> syn::Result<(Ident, TokenStream)> {
        match self.var_type.unwrap() {
            VarTypeInner::SysVar => self.make_sysvar(),
            VarTypeInner::ShowVar => self.make_showvar(),
        }
    }

    fn make_sysvar(&self) -> syn::Result<(Ident, TokenStream)> {
        let Some(vtype) = &self.vtype else {
            return Err(Error::new_spanned(&self.vtype, "missing required field 'vtype'"));
        };
        let str_ty: Expr = parse_quote!(SysVarString);
        let atomic_tys: [Expr; 12] = [
            parse_quote!(AtomicBool),
            parse_quote!(AtomicI8),
            parse_quote!(AtomicI16),
            parse_quote!(AtomicI32),
            parse_quote!(AtomicI64),
            parse_quote!(AtomicIsize),
            parse_quote!(AtomicPtr),
            parse_quote!(AtomicU8),
            parse_quote!(AtomicU16),
            parse_quote!(AtomicU32),
            parse_quote!(AtomicU64),
            parse_quote!(AtomicUsize),
        ];

        // let tmp: proc_macro2::TokenStream = ;
        let name = expect_litstr(&self.name)?;
        let st_ident = Ident::new(&format!("_st_sysvar_{}", name.value()), Span::call_site());

        if vtype == &str_ty {
            Ok((st_ident.clone(), self.make_string_sysvar(&st_ident)?))
        } else if atomic_tys.contains(vtype) {
            Ok((st_ident.clone(), self.make_atomic_sysvar(&st_ident)?))
        } else {
            Err(Error::new_spanned(
                &self.vtype,
                "invalid variable type. Only 'SysVarString' and 'AtomicX' currently allowed.",
            ))
        }
    }

    fn make_string_sysvar(&self, st_ident: &Ident) -> syn::Result<TokenStream> {
        self.validate_correct_fields(STR_REQ_FIELDS, STR_OPT_FIELDS);

        let flags = quote! { ::mariadb::bindings::PLUGIN_VAR_STR as i32 };
        let name = expect_litstr(&self.name)?;
        let description = expect_litstr(&self.description)?;
        let default = if let Some(def) = &self.default {
            quote! { ::mariadb::internals::cstr!(#def).as_ptr().cast_mut() }
        } else {
            quote! { ::mariadb::internals::cstr!("").as_ptr().cast_mut() }
        };
        let disambig_name = quote! { ::std::sync::Mutex<String> };
        let check_fn = quote! { None };
        let update_fn = quote! {
            Some(<::mariadb::plugin::SysVarString as
                ::mariadb::plugin::internals::SimpleSysvarWrap>::update_wrap)
        };
        // let check_fn = quote! { Some(<#disambig_name as ::mariadb::internals::SysVarWrap>::check) };
        // let update_fn =
        //     quote! { Some(<#disambig_name as ::mariadb::internals::SysVarWrap>::update) };
        let ident = self.ident.as_ref().unwrap();
        let res = quote! {
            static #st_ident: ::mariadb::internals::UnsafeSyncCell<
                ::mariadb::bindings::sysvar_str_t,
            > = unsafe {
                ::mariadb::internals::UnsafeSyncCell::new(
                    ::mariadb::bindings::sysvar_str_t {
                        flags: #flags,
                        name: ::mariadb::internals::cstr!(#name).as_ptr(),
                        comment: ::mariadb::internals::cstr!(#description).as_ptr(),
                        check: #check_fn,
                        update: #update_fn,
                        value: ::std::ptr::addr_of!(#ident).cast_mut().cast(), // *mut *mut c_char,
                        def_val: #default,
                    }
                )
            };

        };

        Ok(res)
    }

    fn make_atomic_sysvar(&self, st_ident: &Ident) -> syn::Result<TokenStream> {
        self.validate_correct_fields(NUM_REQ_FIELDS, NUM_OPT_FIELDS);
        todo!()
    }

    fn make_showvar(&self) -> syn::Result<(Ident, TokenStream)> {
        todo!()
    }

    fn validate_correct_fields(&self, required: &[&str], optional: &[&str]) -> syn::Result<()> {
        // These are all required for all plugin types
        let name_map = [
            (&self.ident, "ident"),
            (&self.vtype, "vtype"),
            (&self.name, "name"),
            (&self.description, "description"),
            (&self.options, "options"),
            (&self.default, "default"),
            (&self.min, "min"),
            (&self.max, "max"),
            (&self.interval, "interval"),
        ];
        let vtype = self.vtype.as_ref().unwrap();
        let mut req = REQ_FIELDS.to_vec();
        req.extend_from_slice(required);

        for req_field in &req {
            let (field_val, fname) = name_map.iter().find(|f| f.1 == *req_field).unwrap();

            if field_val.is_none() {
                let msg = format!(
                    "field '{fname}' is expected for variables of type {vtype:?}, but not provided"
                );
                return Err(Error::new(self.span.unwrap(), msg));
            }
        }

        for (field, fname) in name_map {
            if field.is_some() && !req.contains(&fname) && !optional.contains(&fname) {
                let msg =
                    format!("field '{fname}' is not expected for variables of type {vtype:?}");
                return Err(Error::new_spanned(field.as_ref().unwrap(), msg));
            }
        }

        Ok(())
    }
}

fn verify_field_order(fields: &[String]) -> Result<(), String> {
    let mut expected_order = ALL_FIELDS.to_vec();

    expected_order.retain(|expected| fields.iter().any(|f| f == expected));

    if expected_order == fields {
        return Ok(());
    }

    Err(format!(
        "fields not in expected order. reorder as:\n{expected_order:?}",
    ))
}
