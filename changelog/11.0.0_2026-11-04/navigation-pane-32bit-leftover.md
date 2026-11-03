Bugfix: No leftover Explorer entry of a virtual files folder

Windows keeps a copy of the navigation pane entry of a virtual files
folder in the 32-bit registry view
(HKEY_CURRENT_USER\Software\Classes\WOW6432Node\CLSID) when the folder
is unregistered. The copy stayed behind whenever such a folder was
removed, switched to classic synchronization or the client was
uninstalled, with the path of the folder and the icon of the deleted
program. The client and the uninstaller now remove it as well.
