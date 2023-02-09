//! Parse sysvar syntax
//!
//! ```ignore
//! {
//!     ident: SOME_IDENT,
//!     vtype: String,
//!     name: "sql_name",
//!     description: "this is a description",
//!     options: [SysVarOpt::ReadOnly, SysVarOpt::NoCmdOpt],
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
//!     options: [SysVarOpt::ReqCmdArg],
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
use syn::parse::{Parse, ParseBuffer, ParseStream};
use syn::punctuated::Punctuated;
use syn::spanned::Spanned;
use syn::token::Group;
use syn::{
    bracketed, parse_macro_input, parse_quote, Attribute, DeriveInput, Error, Expr, ExprArray,
    ExprLit, ExprStruct, FieldValue, Ident, ImplItem, ImplItemType, Item, ItemImpl, Lit, LitStr,
    Path, PathSegment, Token, Type, TypePath, TypeReference,
};

use crate::fields::sysvar::{ALL_FIELDS, OPT_FIELDS, REQ_FIELDS};
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

        self.validate_correct_fields(REQ_FIELDS, OPT_FIELDS);

        let ty_as_svwrap = quote! { <#vtype as ::mariadb::plugin::internals::SysVarInterface> };
        let name = expect_litstr(&self.name)?;
        let ident = self.ident.as_ref().unwrap();
        let opts = self.make_option_fields()?;
        let flags = quote! { #ty_as_svwrap::DEFAULT_OPTS #opts };
        let description = expect_litstr(&self.description)?;

        let default = process_default_override(&self.default, "def_val")?;
        let min = process_default_override(&self.min, "min_val")?;
        let max = process_default_override(&self.max, "max_val")?;
        let interval = process_default_override(&self.interval, "blk_sz")?;

        let st_ident = Ident::new(&format!("_st_sysvar_{}", name.value()), Span::call_site());
        // https://github.com/rust-lang/rust/issues/86935#issuecomment-1146670057
        let ty_wrap = Ident::new(
            &format!("_st_sysvar_Type{}", name.value()),
            Span::call_site(),
        );

        let usynccell = quote! { ::mariadb::internals::UnsafeSyncCell };

        let res = quote! {
            type #ty_wrap<T> = T;

            static #st_ident: #usynccell<#ty_wrap::<#ty_as_svwrap::CStructType>> = unsafe {
                #usynccell::new(
                    #ty_wrap::<#ty_as_svwrap::CStructType> {
                        flags: #flags,
                        name: ::mariadb::internals::cstr!(#name).as_ptr(),
                        comment: ::mariadb::internals::cstr!(#description).as_ptr(),
                        value: ::std::ptr::addr_of!(#ident).cast_mut().cast(), // *mut *mut c_char,

                        #default
                        #min
                        #max
                        #interval

                        ..#ty_as_svwrap::DEFAULT_C_STRUCT
                        // def_val: #default,
                    }
                )
            };

        };

        Ok((st_ident.clone(), res))
    }

    /// Take the options vector, parse it as an array, bitwise or the output,
    /// cast to i32
    fn make_option_fields(&self) -> syn::Result<TokenStream> {
        let Some(input) = &self.options else {
            return Ok(TokenStream::new());
        };
        let flags: OptFields = syn::parse(input.to_token_stream().into())?;
        let opts = flags.flags;
        if opts.is_empty() {
            return Ok(TokenStream::new());
        }
        let ret = quote! {
            | (( #( #opts .as_plugin_var_info() )|* ) &
                ::mariadb::bindings::PLUGIN_VAR_MASK as i32)
        };
        Ok(ret)
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

#[derive(Clone, Debug)]
struct OptFields {
    flags: Vec<TypePath>,
}

impl Parse for OptFields {
    fn parse(input: ParseStream) -> syn::Result<Self> {
        let content;
        let _ = bracketed!(content in input);
        let flags = Punctuated::<TypePath, Token![,]>::parse_terminated(&content)?;
        let v = Vec::from_iter(flags);
        Ok(Self { flags: v })
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

/// Process "default override" style fields by these rules:
///
/// - If `field` is `None`, return an empty TokenStream
/// - Enforce it is a literal
/// - If it is a literal string, change it to a `cstr`
///
/// Might want to relax and take consts at some point, but that's someday...
fn process_default_override(field: &Option<Expr>, fname: &str) -> syn::Result<TokenStream> {
    let Some(f_inner) = field.as_ref() else {
        return Ok(TokenStream::new());
    };

    let Expr::Lit(exprlit) = f_inner else {
        return Err(Error::new_spanned(f_inner, "only literal values are allowed in this position"));
    };

    let fid = Ident::new(fname, f_inner.span());
    if let syn::Lit::Str(litstr) = &exprlit.lit {
        Ok(quote! { #fid: ::mariadb::internals::cstr!(#litstr).as_ptr().cast_mut(), })
    } else {
        Ok(quote! { #fid: #exprlit, })
    }
}
