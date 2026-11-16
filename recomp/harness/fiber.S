! void fiber_switch(uint32_t save[16], const uint32_t load[16]): r8-r15, pr, mach, macl, fpscr, fr12-fr15
	.text
	.align	2
	.global	_fiber_switch
_fiber_switch:
	mov.l	r8,@(0,r4)
	mov.l	r9,@(4,r4)
	mov.l	r10,@(8,r4)
	mov.l	r11,@(12,r4)
	mov.l	r12,@(16,r4)
	mov.l	r13,@(20,r4)
	mov.l	r14,@(24,r4)
	mov.l	r15,@(28,r4)
	sts	pr,r0
	mov.l	r0,@(32,r4)
	sts	mach,r0
	mov.l	r0,@(36,r4)
	sts	macl,r0
	mov.l	r0,@(40,r4)
	sts	fpscr,r0
	mov.l	r0,@(44,r4)
	mov	r4,r1
	add	#64,r1
	fmov.s	fr15,@-r1
	fmov.s	fr14,@-r1
	fmov.s	fr13,@-r1
	fmov.s	fr12,@-r1

	mov.l	@(44,r5),r0
	lds	r0,fpscr
	mov	r5,r1
	add	#48,r1
	fmov.s	@r1+,fr12
	fmov.s	@r1+,fr13
	fmov.s	@r1+,fr14
	fmov.s	@r1+,fr15
	mov.l	@(32,r5),r0
	lds	r0,pr
	mov.l	@(36,r5),r0
	lds	r0,mach
	mov.l	@(40,r5),r0
	lds	r0,macl
	mov.l	@(0,r5),r8
	mov.l	@(4,r5),r9
	mov.l	@(8,r5),r10
	mov.l	@(12,r5),r11
	mov.l	@(16,r5),r12
	mov.l	@(20,r5),r13
	mov.l	@(24,r5),r14
	rts
	mov.l	@(28,r5),r15
