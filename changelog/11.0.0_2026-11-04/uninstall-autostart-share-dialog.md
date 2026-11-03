Bugfix: Uninstall left the autostart entry behind; share dialog opened too small

The client registers itself for launch on login (HKCU Run key) at first
run; uninstalling never removed that entry, so Windows kept trying to
start the removed client (TC-03 follow-up). The uninstall helper now
deletes both value-name generations. The native share dialog opens at
700px width so the copy/delete buttons of listed links are visible
without manual resizing (TC-07).
