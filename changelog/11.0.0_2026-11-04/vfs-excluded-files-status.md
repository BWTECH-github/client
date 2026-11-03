Bugfix: Excluded files show their state in Explorer with virtual files on Windows

In a folder with virtual files, files that are excluded from syncing (hidden
files, entries of the exclude list) are not Cloud Files placeholders. Explorer
therefore showed no or a misleading sync state for them. The client now
converts such a file into a placeholder that is marked as in sync and
excluded, so Explorer shows it as excluded. A file that another program has
open at that moment (for example an Office lock or temporary file, or the
client's own journal database) is left alone; the conversion is retried on the
next status change. If the file is synced later, the upload sets its file
identity as for any other placeholder.
