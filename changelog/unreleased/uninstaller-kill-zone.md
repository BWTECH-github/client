Bugfix: Uninstalling via Apps & Features removed nothing (7.10.2, TC-03)

The 7.10.2 uninstall helper started the original uninstaller in-place
inside the installation directory. The uninstaller's first step is to
terminate every process running from that directory - silently answered
with "yes, kill" - which terminated both the helper and the uninstaller
itself before anything was removed. Uninstalling via Windows
"Apps & Features" therefore did nothing, while running uninstall.exe
manually worked (started directly, it relocates itself to %TEMP% and runs
outside that kill zone).

The helper now moves its own running image out of the installation
directory (a same-volume rename, fully synchronous - silent callers such
as baramundi keep the real exit code) and runs the original uninstaller as
a %TEMP% copy - both outside the kill zone. Only when the rename is
impossible (e.g. %TEMP% on a different volume) does it fall back to
relaunching a %TEMP% copy of itself, which detaches from the caller. It
verifies afterwards that the client binary is really gone before touching
any registry entries, restores itself when the uninstall failed (so the
Apps & Features entry keeps working for a retry), and reports clear
errors.
