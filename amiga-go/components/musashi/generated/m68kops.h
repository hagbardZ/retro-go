#ifndef M68KOPS__HEADER
#define M68KOPS__HEADER

/* ======================================================================== */
/* ============================ OPCODE HANDLERS =========================== */
/* ======================================================================== */


#if M68K_DYNAMIC_INSTR_TABLES
/* Build the opcode handler table */
void m68ki_build_opcode_table(void);

extern void (*m68ki_instruction_jump_table[0x10000])(void); /* opcode handler jump table */
#if M68K_CYCLE_COUNTING
extern unsigned char m68ki_cycles[][0x10000];
#endif
#else
/* Opcode handler table is already built at compile time */
static inline void m68ki_build_opcode_table(void) {}
extern void (*const m68ki_static_instruction_jump_table[0x10000])(void);

#if M68K_CYCLE_COUNTING
/* Cycle counts per CPU type, generated at build time by m68kmake */
extern const unsigned char m68ki_static_cycles[][0x10000];
#endif
#endif

/* ======================================================================== */
/* ============================== END OF FILE ============================= */
/* ======================================================================== */

#endif /* M68KOPS__HEADER */


