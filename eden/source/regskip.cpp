/* regskip.cpp: bypass the boot-time C++ .eh_frame registration.
 *
 * libXenon's CRT calls c_register_frame() at boot, which points at the
 * executable's .eh_frame and calls __register_frame() -> register_pc_range_
 * for_object() -> get_pc_range() -> classify_object_over_fdes(). On some
 * units that FDE walk is where the console dies (Program exception 0x700)
 * before main() ever runs, so the UI never appears.
 *
 * Eden is built with EDEN_SKIP_REGISTER_FRAME=1 by DEFAULT (see the Makefile),
 * which adds -Wl,--wrap=__register_frame: every reference to __register_frame()
 * is redirected to __wrap___register_frame() below. Making it a no-op skips
 * only the boot-time frame registration; nothing else about the app changes.
 * Eden360 does not use C++ exceptions, so losing the unwinder is harmless, and
 * with this build the menu appears on the units that otherwise fault at boot.
 *
 * This is the shipping behaviour, not a temporary experiment. It is only
 * disabled when explicitly requested with EDEN_SKIP_REGISTER_FRAME=0; in that
 * case this file compiles to nothing and the stock unwinder registration is
 * restored.
 */
#ifdef EDEN_SKIP_REGISTER_FRAME
extern "C" void __wrap___register_frame(void *begin) {
    (void)begin;
}
#endif
