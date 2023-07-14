use proc_macro2::Span;
use syn::{parse_quote, Error, Expr, Ident, Lit, LitStr};

/// Get the field as a boolean
pub fn expect_bool(field_opt: &Option<Expr>) -> syn::Result<bool> {
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
pub fn expect_litstr(field_opt: &Option<Expr>) -> syn::Result<&LitStr> {
    let field = field_opt.as_ref().unwrap();
    let Expr::Lit(lit) = field else {
        // got non-literal
        let msg = "expected literal expression for this field";
        return Err(Error::new_spanned(field, msg));
    };
    let Lit::Str(litstr) = &lit.lit else {
        // got literal that wasn't a string
        let msg = "only literal strings are allowed for this field";
        return Err(Error::new_spanned(field, msg));
    };

    Ok(litstr)
}

/// Create an identifier from a string with span at the macro call site
pub fn make_ident(s: &str) -> Ident {
    Ident::new(s, Span::call_site())
}
