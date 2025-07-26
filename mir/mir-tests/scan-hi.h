MIR_module_t create_hi_module (MIR_context_t ctx) {
  const char *str
    = "\n\
m_hi:    module\n\
proto:	 proto i32, i32:ch\n\
	 import print\n\
         export hi\n\
hi:      func i32\n\
         local i64:h, i64:i, i64:exc, i64:nl, i64:r, i64:temp\n\
         mov h, 104\n\
         call proto, print, r, h\n\
         mov i, 105\n\
         call proto, print, temp, i\n\
         add r, r, temp\n\
         mov exc, 33\n\
         call proto, print, temp, exc\n\
         add r, r, temp\n\
         mov nl, 10\n\
         call proto, print, temp, nl\n\
         add r, r, temp\n\
         ret r\n\
         endfunc\n\
         endmodule\n\
";

  MIR_scan_string (ctx, str);
  return DLIST_TAIL (MIR_module_t, *MIR_get_module_list (ctx));
}
