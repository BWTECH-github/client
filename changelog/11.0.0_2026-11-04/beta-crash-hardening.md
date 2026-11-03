Bugfix: Unusual server answers and system states no longer end the client

Beta builds stop at every violated internal check. Many of these checks
could be triggered from outside, so the client ended instead of reporting a
problem. Now handled and logged: missing or already removed keychain entries
when signing out or removing an account, a browser connection that closes
before the sign-in callback is read, sign-in services announcing unknown
prompt values, quitting while a WebFinger lookup, a confirmation or the
removal of a folder is still running, a sync root that cannot be created or
is already marked, switching to another account while a sign-in dialog
finishes, server dates in an unexpected format, empty etags, depth 0
listings with several entries, server paths outside the requested folder,
URLs with "//" or a query, negative sizes, an unwritable resources cache,
files locked by other programs when setting their modification time, failed
writes to the sync journal, malformed messages from other local programs on
the shell integration socket, retrying a failed update after a restart, and
on Windows refused shutdown blocks, partial hydration requests and
placeholder records sharing a file id. A deleted item replaced by a rename
in an unexpected state is now resolved as a rename instead of ending every
sync (this also affected release builds). Error texts from the sign-in
service can no longer make the error page loop or abort.
