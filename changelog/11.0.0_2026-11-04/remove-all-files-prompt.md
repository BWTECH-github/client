Enhancement: Ask before a sync deletes all or very many files

As in the 2.x client, a sync that would delete all files of a folder (for
example because the local folder is empty, a drive is not mounted or the
server storage is broken) stops before changing anything and asks: "Keep
files" restores the deleted files from the other side, "Delete files" lets
the sync go ahead. The folder stays paused until the question is answered.

New compared to 2.x: the question also comes when at least 200 files and at
least half of the known files would be deleted, "Keep files" only restores
the reported files (other changes are synced as usual), and placeholders of
virtual files without local content do not count. The config key
promptDeleteAllFiles=false turns the question off. owncloudcmd asks on the
terminal; with --non-interactive it stops with exit code 3 without deleting.
