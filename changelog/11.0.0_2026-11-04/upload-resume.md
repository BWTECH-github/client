Bugfix: Interrupted large uploads restarted from scratch (TC-14)

A connection loss during a large chunked upload frequently threw away all
progress instead of resuming: the retry run inside the error-backoff window
wiped the stored upload session from the journal (restart at byte 0), a
transient error during the resume lookup discarded the server-side session
unnecessarily, completed chunks after a gap were deleted from the server
instead of being reused, and the retry backoff escalated up to 24 hours -
together these caused the "complete sync restart with a very long
recalculation" the QA observed.

Fixed: the upload session survives temporary blacklisting, the resume
lookup only starts over when the server reports the session gone (404/410),
completed chunks after a gap are kept, and interrupted uploads retry within
60 seconds like interrupted downloads already did (quota-exceeded and
locked-file errors keep the normal escalating backoff). The content
checksum is still freshly computed on each attempt - that is deliberate, it
is the integrity check that protects resumed chunks against files edited in
place - but with working resume it costs seconds, not a restart from zero.
A release-build crash with servers that advertise no checksum type was
fixed along the way (such servers now resume based on size and modification
time), and HTTP/2 is back to opt-in (off by default), matching its
documentation - it multiplied parallel chunk loss on interrupted transfers
and can stall with some proxy setups.
