// libultra builds seen only in ROMs, not in the objects the other files come from: written by hand, same syntax as
// patch_symbols.inl

// A pre-2.0D __osRestoreInt with a hazard nop after the mfc0 (in the Cruis'n USA ROM at 0x80111130)
BEGIN_PATCH_XREFS(__osRestoreInt)
END_PATCH_XREFS()
BEGIN_PATCH_SIGNATURE_LIST(__osRestoreInt)
   PATCH_SIGNATURE_LIST_ENTRY(__osRestoreInt, 8, 16, 0xbe6c6b35, 0x82ec90d6)
END_PATCH_SIGNATURE_LIST()
PATCH_SYMBOL_FUNCTION(__osRestoreInt)
