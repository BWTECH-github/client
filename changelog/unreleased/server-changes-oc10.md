Bugfix: Changes on owncloud.online servers arrive within 30 seconds

Without Spaces (owncloud.online and ownCloud 10) nothing told the client
about changes on the server: only Spaces report a changed ETag. A file
created on the server, for example in the web interface or by another
device, therefore only arrived with the full synchronization roughly
once an hour, or with the next local change.

The client now asks for the ETag of the server folder of every folder
sync connection without a Space every 30 seconds (remotePollInterval),
like the classic client did, and synchronizes the folder when it changed.
It asks only after the first synchronization of the folder, never while
the folder is synchronizing, and at most once at a time per folder.
