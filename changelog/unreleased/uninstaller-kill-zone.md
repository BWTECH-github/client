Bugfix: Uninstalling via Apps & Features removed nothing (7.10.2, TC-03)

The 7.10.2 uninstall helper lived inside the installation directory and
started the original uninstaller in-place there. The uninstaller's first
step terminates every process running from that directory - silently
answered with "yes, kill" - which terminated both the helper and the
uninstaller itself before anything was removed. Uninstalling via Windows
"Apps & Features" therefore did nothing, while running uninstall.exe
manually worked (started directly, it relocates itself to %TEMP% and runs
outside that kill zone).

The helper is now installed to C:\ProgramData\owncloud.online - outside
the kill zone by construction - and runs the original uninstaller as a
%TEMP% copy, so neither process can be terminated by the process check.
The flow stays fully synchronous for silent callers (real exit codes:
2 on failure, 3010 for "reboot required" with /S /norestart). The helper
additionally verifies afterwards that the client binary is really gone
before touching any registry entries, and reports a clear error
otherwise. Upgrading to 7.10.3 replaces the misplaced 7.10.2 helper and
repoints the Apps & Features entry.
