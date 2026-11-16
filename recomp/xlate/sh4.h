#ifndef SH4_H
#define SH4_H

// SH-4 encoders, operands in gas order (op Rm,Rn): n in bits 8-11, m in bits 4-7
#define NM(base, m, n)        ((uint16_t)((base) | ((n) << 8) | ((m) << 4)))
#define N_(base, n)           ((uint16_t)((base) | ((n) << 8)))

#define MOVI(i, n)            ((uint16_t)(0xE000 | ((n) << 8) | ((i) & 0xFF)))
#define MOV(m, n)             NM(0x6003, m, n)
#define MOVL_LDD(d, m, n)     ((uint16_t)(0x5000 | ((n) << 8) | ((m) << 4) | ((d) >> 2)))  // mov.l @(d,Rm),Rn
#define MOVL_STD(m, d, n)     ((uint16_t)(0x1000 | ((n) << 8) | ((m) << 4) | ((d) >> 2)))  // mov.l Rm,@(d,Rn)
#define MOVL_LD0(m, n)        NM(0x000E, m, n)   // mov.l @(r0,Rm),Rn
#define MOVW_LD0(m, n)        NM(0x000D, m, n)
#define MOVB_LD0(m, n)        NM(0x000C, m, n)
#define MOVL_ST0(m, n)        NM(0x0006, m, n)   // mov.l Rm,@(r0,Rn)
#define MOVW_ST0(m, n)        NM(0x0005, m, n)
#define MOVB_ST0(m, n)        NM(0x0004, m, n)
#define MOVL_PUSH(m, n)       NM(0x2006, m, n)   // mov.l Rm,@-Rn
#define MOVL_POP(m, n)        NM(0x6006, m, n)   // mov.l @Rm+,Rn
#define MOVL_LDI(m, n)        NM(0x6002, m, n)   // mov.l @Rm,Rn
#define MOVL_STI(m, n)        NM(0x2002, m, n)   // mov.l Rm,@Rn
#define MOVL_PC(d4, n)        ((uint16_t)(0xD000 | ((n) << 8) | (d4)))
#define FMOVS_LD0(m, n)       NM(0xF006, m, n)
#define FMOVS_ST0(m, n)       NM(0xF007, m, n)
#define FMOV(m, n)            NM(0xF00C, m, n)
#define FMOVS_PUSH(m, n)      NM(0xF00B, m, n)
#define FMOVS_POP(m, n)       NM(0xF009, m, n)
#define ADDI(i, n)            ((uint16_t)(0x7000 | ((n) << 8) | ((i) & 0xFF)))
#define ADD(m, n)             NM(0x300C, m, n)
#define SUB(m, n)             NM(0x3008, m, n)
#define AND(m, n)             NM(0x2009, m, n)
#define OR(m, n)              NM(0x200B, m, n)
#define XOR(m, n)             NM(0x200A, m, n)
#define XORI0(i)              ((uint16_t)(0xCA00 | ((i) & 0xFF)))
#define NOT(m, n)             NM(0x6007, m, n)
#define CMPGT(m, n)           NM(0x3007, m, n)
#define CMPGE(m, n)           NM(0x3003, m, n)
#define CMPHI(m, n)           NM(0x3006, m, n)
#define CMPEQ(m, n)           NM(0x3000, m, n)
#define CMPPL(n)              N_(0x4015, n)
#define CMPPZ(n)              N_(0x4011, n)
#define TST(m, n)             NM(0x2008, m, n)
#define MOVT(n)               N_(0x0029, n)
#define SHLL(n)               N_(0x4000, n)
#define SHLR(n)               N_(0x4001, n)
#define SHAR(n)               N_(0x4021, n)
#define SHLL2(n)              N_(0x4008, n)
#define SHLR2(n)              N_(0x4009, n)
#define SHLL8(n)              N_(0x4018, n)
#define SHLR8(n)              N_(0x4019, n)
#define SHLL16(n)             N_(0x4028, n)
#define SHLR16(n)             N_(0x4029, n)
#define SHLD(m, n)            NM(0x400D, m, n)
#define SHAD(m, n)            NM(0x400C, m, n)
#define NEG(m, n)             NM(0x600B, m, n)
#define DMULS(m, n)           NM(0x300D, m, n)
#define DMULU(m, n)           NM(0x3005, m, n)
#define MULL(m, n)            NM(0x0007, m, n)
#define STS_MACL(n)           N_(0x001A, n)
#define STS_MACH(n)           N_(0x000A, n)
#define STS_FPUL(n)           N_(0x005A, n)
#define STS_FPSCR(n)          N_(0x006A, n)
#define LDS_FPUL(m)           N_(0x405A, m)
#define LDS_FPSCR(m)          N_(0x406A, m)
#define LDSL_PR_POP(m)        N_(0x4026, m)
#define STSL_PR_PUSH(n)       N_(0x4022, n)
#define FLDS(m)               N_(0xF01D, m)
#define FSTS(n)               N_(0xF00D, n)
#define FADD(m, n)            NM(0xF000, m, n)
#define FSUB(m, n)            NM(0xF001, m, n)
#define FMUL(m, n)            NM(0xF002, m, n)
#define FDIV(m, n)            NM(0xF003, m, n)
#define FNEG(n)               N_(0xF04D, n)
#define FABS(n)               N_(0xF05D, n)
#define FSQRT(n)              N_(0xF06D, n)
#define FLOAT(n)              N_(0xF02D, n)
#define FTRC(m)               N_(0xF03D, m)
#define FCNVSD(n)             N_(0xF0AD, n)
#define FCNVDS(m)             N_(0xF0BD, m)
#define FCMPGT(m, n)          NM(0xF005, m, n)
#define FCMPEQ(m, n)          NM(0xF004, m, n)
#define EXTUW(m, n)           NM(0x600D, m, n)
#define EXTUB(m, n)           NM(0x600C, m, n)
#define JSR(n)                N_(0x400B, n)
#define JMP(n)                N_(0x402B, n)
#define DT(n)                 N_(0x4010, n)
#define RTS                   ((uint16_t)0x000B)
#define NOP                   ((uint16_t)0x0009)
#define BT(d)                 ((uint16_t)(0x8900 | ((d) & 0xFF)))
#define BF(d)                 ((uint16_t)(0x8B00 | ((d) & 0xFF)))
#define BRA(d)                ((uint16_t)(0xA000 | ((d) & 0xFFF)))

// Whether op writes T, for the forms above
static inline int sh_clobbers_t(uint16_t op) {
    uint16_t a = op & 0xF00F, b = op & 0xF0FF;
    return a == 0x3000 || a == 0x3006 || a == 0x3007 || a == 0x2008 || a == 0xF004 || a == 0xF005 ||
           b == 0x4015 || b == 0x4011 || b == 0x400B || b == 0x4010 || b == 0x4000 || b == 0x4001 || b == 0x4021;
}

#endif
