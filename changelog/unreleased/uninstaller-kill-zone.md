Bugfix: Uninstalling via Apps & Features removed nothing (7.10.2, TC-03)

The 7.10.2 uninstall helper started the original uninstaller in-place
inside the installation directory. The uninstaller's first step is to
terminate every process running from that directory - silently answered
with "yes, kill" - which terminated both the helper and the uninstaller
itself before anything was removed. Uninstalling via Windows
"Apps & Features" therefore did nothing, while running uninstall.exe
manually worked (started directly, it relocates itself to %TEMP% and runs
outside that kill zone).

The helper now relaunches itself from %TEMP% and also runs the original
uninstaller as a %TEMP% copy - both outside the kill zone - keeping the
real wait-and-exit-code behavior. It additionally verifies afterwards that
the client binary is really gone before touching any registry entries, and
reports a clear error otherwise.
