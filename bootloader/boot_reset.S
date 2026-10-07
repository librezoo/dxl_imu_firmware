/* Reset handler of the bootloader. It replaces the weak handle_reset of the WCH startup file, which returns
   to main in user mode (mstatus.MPP = 0). The bootloader stays in machine mode so that it can hand the CPU
   to an application's own reset code, which writes machine-mode CSRs. */
	.section	.text.handle_reset,"ax",@progbits
	.global	handle_reset
	.align	1
handle_reset:
.option push
.option	norelax
	la gp, __global_pointer$
.option	pop
	la sp, _eusrstack

	la a0, _data_lma
	la a1, _data_vma
	la a2, _edata
	bgeu a1, a2, 2f
1:
	lw t0, (a0)
	sw t0, (a1)
	addi a0, a0, 4
	addi a1, a1, 4
	bltu a1, a2, 1b
2:
	la a0, _sbss
	la a1, _ebss
	bgeu a0, a1, 2f
1:
	sw zero, (a0)
	addi a0, a0, 4
	bltu a0, a1, 1b
2:
	li t0, 0x1f
	csrw 0xbc0, t0
	li t0, 0x3
	csrw 0x804, t0
	li t0, 0x1808         /* machine mode after mret, global interrupts stay off */
	csrw mstatus, t0
	la t0, _vector_base
	ori t0, t0, 3
	csrw mtvec, t0

	jal  SystemInit
	la t0, main
	csrw mepc, t0
	mret
