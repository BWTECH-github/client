Bugfix: Resumed uploads looked like complete restarts (TC-14)

Three follow-ups to the 7.10.2 resume fixes: (1) Reused chunks are now
reported to the UI immediately after the resume inventory - previously the
progress stayed at zero until the first NEW chunk completed, which at
throttled speeds took minutes and made a successful resume look like a
full restart. (2) A transiently unavailable checksum (e.g. the file was
briefly unreadable while hashing) no longer marks the
upload session as stale - that path DELETED the server-side chunks of a
perfectly resumable upload and forced a restart from zero; size and
modification time are used for validation whenever a checksum comparison
is impossible. (3) When the server itself reports the upload session gone
(HTTP 404/410), a log line now says so explicitly, so a genuine
server-side session expiry can be told apart from a client-side discard
in QA logs.
