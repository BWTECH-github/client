Bugfix: Uploads from the Finder show their progress on macOS

Files copied into the folder in the Finder were uploaded without any visible
progress: the Finder showed only a clock until each file was done, and the
folder view of the client still said "Synced". The File Provider extension now
reports the bytes it sends, so the Finder shows the progress of each upload,
and the folder view shows the uploads and downloads of the Finder with the
progress bar and the transfer rate, also while many files are copied at once,
and the remaining time while a single file is transferred. A cancelled upload
stops right away, and an upload the system repeats no longer creates a second
copy of a file the server already has.
