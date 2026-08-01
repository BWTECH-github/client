Bugfix: Interrupted large uploads restarted from scratch (TC-14)

A connection loss during a large chunked upload frequently threw away all
progress instead of resuming: the retry run inside the error-backoff window
wiped the stored upload session from the journal (restart at byte 0 with a
new transfer id), a transient error during the resume lookup discarded the
server-side session unnecessarily, completed chunks after a gap were
deleted from the server instead of being reused, the multi-gigabyte
checksum was recomputed from scratch on every attempt (the "long
recalculation"), and the retry backoff escalated up to 24 hours.

All five defects are fixed: the upload session survives temporary
blacklisting, the resume lookup only starts over when the server reports
the session gone (404/410), completed chunks after a gap are kept, the
stored checksum is reused while size and modification time are unchanged,
and interrupted uploads retry within 60 seconds like interrupted downloads
already did. A release-build crash with servers that advertise no checksum
type was fixed along the way, and HTTP/2 is back to opt-in (off by
default), matching its documentation - it multiplied parallel chunk loss
on interrupted transfers and can stall with some proxy setups.
