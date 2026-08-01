Bugfix: Uninstalling left shortcuts and the Explorer integration behind (TC-03)

Uninstalling via Windows "Apps & Features" removed the program but kept the
desktop and start menu shortcuts, left the Explorer shell extensions
(overlay icons, context menu) registered and loaded, and never asked for
the Windows restart needed to unload them.

The setup now installs a branded uninstall helper and routes the
Apps & Features entry through it: it stops the client, unregisters the
shell extensions, runs the original uninstaller, removes the shortcuts
(all users and current user), best-effort removes the navigation-pane
registry entries, cleans leftover files and registry keys in both registry
views, and offers the required Windows restart (interactive) or reboots
automatically (silent /S; suppress with /norestart, which then exits with
code 3010 so deployment tools can schedule the reboot). The local sync
folder and account settings remain untouched, as before.
