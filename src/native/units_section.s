// Ensures __DATA,__m2m_units exists even when no translation units are
// linked (the runtime reads its bounds via section$start/section$end).
.section __DATA,__m2m_units
.p2align 3
