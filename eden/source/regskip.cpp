/* regskip.cpp: build-test hook to bypass the boot-time C++ .eh_frame
 * registration.
 *
 * libXenon's CRT calls c_register_frame() at boot, which points at the
 * executable's .eh_frame and calls __register_frame() -> register_pc_range_
 * for_object() -> get_pc_range() -> classify_object_over_fdes(). On some
 * units that FDE walk is where the console dies (Program exception 0x700)
 * before main() ever runs, so the UI never appears.
 *
 * Building eden with EDEN_SKIP_REGISTER_FRAME=1 (see the Makefile) adds
 * -Wl,--wrap=__register_frame here, which redirects every reference to
 * __register_frame() to __wrap___register_frame() below. Making it a no-op
 * skips only the boot-time frame registration; nothing else about the app
 * changes. This is a diagnostic/experiment build: if the menu then shows, the
 * crash was that walk. C++ exceptions would no longer unwind, which eden does
 * not rely on.
 *
 * With the macro unset this file compiles to nothing and the normal build is
 * byte-for-byte unaffected.
 */
#ifdef EDEN_SKIP_REGISTER_FRAME
extern "C" void __wrap___register_frame(void *begin) {
    (void)begin;
}
#endif
