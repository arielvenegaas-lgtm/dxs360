# Temporary program initialisation and shutdown code for FreeChainXenon
#
# Copyright (c) 2025 Aiden Isik
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

.global _start
.global __eabi

.text

# Stub. The compiler will complain that this is missing otherwise.
# We need a C library working to use the true __eabi, so here we can ONLY write code in main().
__eabi:
	blr

_start:
	# Save the non-volatile integer registers
	std r2, -0x8(r1)
	std r14, -0x10(r1)
	std r15, -0x18(r1)
	std r16, -0x20(r1)
	std r17, -0x28(r1)
	std r18, -0x30(r1)
	std r19, -0x38(r1)
	std r20, -0x40(r1)
	std r21, -0x48(r1)
	std r22, -0x50(r1)
	std r23, -0x58(r1)
	std r24, -0x60(r1)
	std r25, -0x68(r1)
	std r26, -0x70(r1)
	std r27, -0x78(r1)
	std r28, -0x80(r1)
	std r29, -0x88(r1)
	std r30, -0x90(r1)
	std r31, -0x98(r1)

	# Save the non-volatile float registers
	stfd f14, -0xA0(r1)
	stfd f15, -0xA8(r1)
	stfd f16, -0xB0(r1)
	stfd f17, -0xB8(r1)
	stfd f18, -0xC0(r1)
	stfd f19, -0xC8(r1)
	stfd f20, -0xD0(r1)
	stfd f21, -0xD8(r1)
	stfd f22, -0xE0(r1)
	stfd f23, -0xE8(r1)
	stfd f24, -0xF0(r1)
	stfd f25, -0xF8(r1)
	stfd f26, -0x100(r1)
	stfd f27, -0x108(r1)
	stfd f28, -0x110(r1)
	stfd f29, -0x118(r1)
	stfd f30, -0x120(r1)
	stfd f31, -0x128(r1)

	# Save the link register
	mflr r14
	std r14, -0x130(r1)

	# Determine where the stack pointer should be moved to (16-byte alignment down, at least 8 bytes free)
	subi r14, r1, 0x140
	lis r15, 0xFFFF
	ori r15, r15, 0xFFF0
	and r14, r14, r15

	# Save the stack pointer, then update it
	std r1, -0x8(r14)
	mr r1, r14

	# Call main()
	bl main

	# Restore the stack pointer
	ld r1, -0x8(r1)

	# Restore the link register
	ld r14, -0x130(r1)
	mtlr r14

	# Restore the non-volatile float registers
	lfd f14, -0xA0(r1)
	lfd f15, -0xA8(r1)
	lfd f16, -0xB0(r1)
	lfd f17, -0xB8(r1)
	lfd f18, -0xC0(r1)
	lfd f19, -0xC8(r1)
	lfd f20, -0xD0(r1)
	lfd f21, -0xD8(r1)
	lfd f22, -0xE0(r1)
	lfd f23, -0xE8(r1)
	lfd f24, -0xF0(r1)
	lfd f25, -0xF8(r1)
	lfd f26, -0x100(r1)
	lfd f27, -0x108(r1)
	lfd f28, -0x110(r1)
	lfd f29, -0x118(r1)
	lfd f30, -0x120(r1)
	lfd f31, -0x128(r1)

	# Restore the non-volatile integer registers
	ld r2, -0x8(r1)
	ld r14, -0x10(r1)
	ld r15, -0x18(r1)
	ld r16, -0x20(r1)
	ld r17, -0x28(r1)
	ld r18, -0x30(r1)
	ld r19, -0x38(r1)
	ld r20, -0x40(r1)
	ld r21, -0x48(r1)
	ld r22, -0x50(r1)
	ld r23, -0x58(r1)
	ld r24, -0x60(r1)
	ld r25, -0x68(r1)
	ld r26, -0x70(r1)
	ld r27, -0x78(r1)
	ld r28, -0x80(r1)
	ld r29, -0x88(r1)
	ld r30, -0x90(r1)
	ld r31, -0x98(r1)

	# Exit
	# NOTE: This is not how we exit a program on 360.
	blr
