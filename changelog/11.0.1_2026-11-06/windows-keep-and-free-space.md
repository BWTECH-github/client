Bugfix: Files stay in place when you keep them or free up space under Windows

"Always keep on this device" downloaded a file by replacing it, so the entry
disappeared for a moment from an open Explorer window. Files are now downloaded
in place, the entry stays the same file. Explorer shows the new state right after
a file was downloaded or its space was freed instead of "Sync pending". The folder
view of the client updates when the sync is done: it kept showing "Currently
available locally" for a file that was already online only. A choice made in the
client runs at once, a choice for a folder with thousands of files in Explorer no
longer gets lost when the change notifications overflow, and "Always available
locally" no longer depends on the search index.
