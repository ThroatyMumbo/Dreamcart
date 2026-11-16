! Dreamcart 64's ELF entry and its way back in: .data is snapshotted before KOS first runs, restored by dc64_restart.c
    .section .rodata.dc64, "a"
    .balign 32
    .global _dc64_pristine, _dc64_pristine_end, _dc64_boots, _dc64_note
_dc64_pristine:
    .space 0x8000
_dc64_pristine_end:
_dc64_boots:
    .long 0
_dc64_note:
    .space 48

    .text
    .balign 4
    .global _dc64_entry
! dcload's sr, pr and r15 pass through untouched; the copy goes through P2, since KOS's start invalidates the D-cache
_dc64_entry:
    mov.l   .Llo, r0
    mov.l   .Lhi, r1
    mov.l   .Lbuf, r2
    mov.l   .Lbufend, r3
    mov     r1, r4
    sub     r0, r4
    mov     r3, r5
    sub     r2, r5
    cmp/hi  r5, r4
    bt      2f
    mov.l   .Lp2, r5
    or      r5, r2
1:  cmp/hs  r1, r0
    bt      2f
    mov.l   @r0+, r3
    mov.l   r3, @r2
    bra     1b
    add     #4, r2
2:  mov.l   .Lstart, r0
    jmp     @r0
    nop

    .global _dc64_reenter
! arch_real_exit without its dcload exit call: dcload's sr, MMU off, dcload's vbr, stack and pr, then KOS's start again
_dc64_reenter:
    mov.l   .Lold_sr, r0
    mov.l   @r0, r0
    ldc     r0, sr
    mov     #4, r0
    mov.l   .Lmmucr, r1
    mov.l   r0, @r1
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    mov.l   .Lold_vbr, r0
    mov.l   @r0, r0
    ldc     r0, vbr
    mov.l   .Lold_stack, r0
    mov.l   @r0, r15
    lds.l   @r15+, pr
    mov.l   .Lstart, r0
    jmp     @r0
    nop

    .balign 4
.Llo:        .long _dc64_data_lo
.Lhi:        .long _edata
.Lbuf:       .long _dc64_pristine
.Lbufend:    .long _dc64_pristine_end
.Lp2:        .long 0xa0000000
.Lstart:     .long _start
.Lold_sr:    .long __arch_old_sr
.Lold_vbr:   .long __arch_old_vbr
.Lold_stack: .long __arch_old_stack
.Lmmucr:     .long 0xff000010
