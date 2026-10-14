Bugfix: Setup touched the official ownCloud client and restarted Windows in silent mode (7.8.0-7.11.0)

Two things the Windows setup and the uninstall helper did could hit a machine
that also has the official ownCloud client installed: the setup removed
"C:\Program Files\ownCloud" recursively as soon as it found files there and
the new client lived elsewhere - without checking what that directory
contained - and both stopped owncloud.exe, deleted "ownCloud" shortcuts and
the "ownCloud" autostart entry "defensively". On a machine with the official
client (MSI 6.x or craft 7.x, which lives exactly there) that destroyed that
installation.

The old directory is now touched only when it demonstrably holds an old
installation of THIS client (bin\owncloud.online.exe, versions 7.4-7.7);
otherwise it is left untouched and the setup says so in its log. Only the
own process name (owncloud.online.exe), the own shortcuts and the own
autostart entry are stopped or removed; the broken "ownCloud" Start menu
entry the embedded base installer creates is removed only if it did not
exist before the setup ran. The uninstall helper runs the embedded
uninstaller, unregisters the shell extensions and removes the directory only
for a directory holding this client, and keeps the shared registry entries
when they describe the official client.

Silent installs and uninstalls (/S, e.g. baramundi, Intune, SCCM) no longer
restart Windows on their own. They return exit code 3010 when a restart is
pending (the msiexec convention deployment tools understand; set only when a
locked file had to be scheduled for the next restart) and 0 otherwise.
/S /forcerestart restarts right away; /S /norestart is still accepted and
behaves like plain /S. Interactive installs keep offering the restart on the
finish page.

The release no longer publishes the raw base installer
(owncloud.online-client-<version>-win-x64.exe): it carried the upstream
identity ("ownCloud" / "ownCloud GmbH" in Apps & Features, a Start menu entry
pointing at a non-existent bin\owncloud.exe, no Explorer integration). The
Setup is the only Windows installer offered.
