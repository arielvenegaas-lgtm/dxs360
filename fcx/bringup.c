/*
 * Aurora Bricks - FreeChainXenon bring-up program.
 *
 * FreeChainXenon (FCX) is the only free/open-source toolchain that produces a
 * genuine Xbox 360 `.xex` (via SynthXEX). It is very early: it has no D3D9, no
 * input and no graphics API, so the game itself cannot be rendered yet. What it
 * CAN do is build and run a real title-mode XEX on the console's OS, which is
 * exactly what this bring-up proves.
 *
 * Everything lives in main() on purpose: FCX's C front-end is still rough and
 * (per its own docs) function calls / locals can be unreliable. No libc, no
 * includes - freestanding code only.
 *
 * It resolves VdDisplayFatalError by ordinal out of the kernel's PE export
 * table (the same technique used by the upstream FCX example) and calls it,
 * which paints a visible error code. Seeing a code on screen proves the XEX
 * booted and executed. (The code below is arbitrary; 0x1244A matches the
 * upstream demo.)
 */

int main(void)
{
    unsigned int i;
    unsigned int v;
    unsigned int pe;
    unsigned int expdir;
    unsigned int eat;
    unsigned int addr;
    unsigned int ordinal = 0x1B2u; /* VdDisplayFatalError */

    /* PE header RVA lives at KB base 0x80040000 + 0x3C. */
    v = 0;
    for (i = 0; i < 4u; i++)
        v |= ((unsigned int)(*(volatile unsigned char *)(0x8004003Cu + i))) << (i * 8u);
    pe = v + 0x80040000u;

    /* Export directory table: PE header + 0x78. */
    v = 0;
    for (i = 0; i < 4u; i++)
        v |= ((unsigned int)(*(volatile unsigned char *)(pe + 0x78u + i))) << (i * 8u);
    expdir = v + 0x80040000u;

    /* Export address table RVA: export dir + 0x1C. */
    v = 0;
    for (i = 0; i < 4u; i++)
        v |= ((unsigned int)(*(volatile unsigned char *)(expdir + 0x1Cu + i))) << (i * 8u);
    eat = v + 0x80040000u;

    /* Address of our ordinal (EAT is 0-based on ordinal-1). */
    v = 0;
    for (i = 0; i < 4u; i++)
        v |= ((unsigned int)(*(volatile unsigned char *)(eat + (ordinal - 1u) * 4u + i))) << (i * 8u);
    addr = v + 0x80040000u;

    if (addr != 0u) {
        typedef void (*fatal_fn)(unsigned int);
        fatal_fn fn = (fatal_fn)addr;
        fn(0x1244Au); /* show a code: proof that the title ran */
    }

    /* Park here if we somehow come back. */
    for (;;) { }

    return 0;
}
