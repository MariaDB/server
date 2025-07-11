MIR_module_t create_args_module (MIR_context_t ctx) {
  const char *str
    = "\n\
m:     module\n\
p_pri: proto i64:v\n\
p_prf: proto f:v\n\
p_prd: proto d:v\n\
       import pri, prf, prd\n\
       export f\n\
f:     func i64, i8:i1, i16:i2, i32:i3, i64:i4, f:f1, d:d1, u32:i5, u8:i6, u16:i7, i32:i8, i64:i9, f:f2, f:f3, f:f4, f:f5, f:f6, f:f7, f:f8, d:d2\n\
       call p_pri, pri, i1\n\
       call p_pri, pri, i2\n\
       call p_pri, pri, i3\n\
       call p_pri, pri, i4\n\
       call p_pri, pri, i5\n\
       call p_pri, pri, i6\n\
       call p_pri, pri, i7\n\
       call p_pri, pri, i8\n\
       call p_pri, pri, i9\n\
       call p_prf, prf, f1\n\
       call p_prf, prf, f2\n\
       call p_prf, prf, f3\n\
       call p_prf, prf, f4\n\
       call p_prf, prf, f5\n\
       call p_prf, prf, f6\n\
       call p_prf, prf, f7\n\
       call p_prf, prf, f8\n\
       call p_prd, prd, d1\n\
       call p_prd, prd, d2\n\
       ret 0\n\
       endfunc\n\
       endmodule\n\
";
  MIR_scan_string (ctx, str);
  return DLIST_TAIL (MIR_module_t, *MIR_get_module_list (ctx));
}
