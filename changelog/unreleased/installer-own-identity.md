Change: Windows installer with its own identity and uninstall choices

The embedded installer is now built from our own craft blueprint fork
(BWTECH-github/craft-blueprints-owncloud, branch bwtech): Apps & Features shows
"owncloud.online" by "BW-Tech GmbH" with the owncloud.online link, the Start
menu entry lives in its own "owncloud.online" folder and points at the real
client, and the registry keys are Software\BW-Tech GmbH\owncloud.online and
Uninstall\owncloud.online. The Explorer overlay and context menu handlers use
their own CLSIDs and names, so they no longer overwrite or remove the
registration of the official ownCloud client.

Updating from 7.x moves over cleanly: the old "ownCloud" uninstall entry and
registry key are removed when they belong to this client (the official 6.x MSI
would otherwise run our uninstaller as its "legacy installation"), as are the
old Explorer registrations pointing into our folder and the old Start menu
shortcut. Shell extension DLLs that Explorer keeps loaded are moved aside so
the new ones are installed.

Uninstalling asks what to remove besides the program: settings, accounts and
credentials (on by default) and sync databases and logs in the sync folders
(off by default). The sync roots of the client are unregistered, so online-only
placeholders disappear while downloaded files stay; the files in the sync
folders are never deleted. A silent uninstall (/S) keeps settings, accounts
and credentials, /S /purge removes them.
