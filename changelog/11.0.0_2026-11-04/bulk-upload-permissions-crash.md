Bugfix: Files uploaded in bulk are recorded with their permissions

Against servers that offer the bulk upload endpoint (owncloud.online 11.0.14
or later), new files of up to 1 MiB are uploaded in batches. The server's
answer carries no file permissions, but the sync journal requires them: the
files were recorded without permissions, and builds with assertions enabled
terminated on the first batch.

The client now looks up the permissions of every uploaded file with one small
PROPFIND, the same lookup a single upload does, and keeps no more of these
requests in flight than it allows parallel transfers. If a lookup fails, the
file stays on the server and the next sync records it without uploading it
again.
