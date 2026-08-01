Bugfix: Bandwidth limit could block all transfers; download checkbox did not stick (TC-16)

Two defects in the bandwidth limiter: (1) When a request was retried (e.g.
after a token refresh or an HTTP/2 resend), the upload device had already
unregistered itself from the bandwidth manager and never received transfer
quota again - the retried upload stalled at zero bytes, occupied a
transfer slot for its full five-minute timeout and thereby blocked
downloads too. Devices now stay registered for their whole lifetime, and
the per-tick budget is divided only among consumers that can actually use
it. (2) Opening the network settings silently reverted the "limit download
bandwidth" checkbox: loading the upload checkbox fired an immediate save
that overwrote the not-yet-loaded download setting. Saving now only
happens on real user clicks.

Two crash-on-release landmines in the same code paths (asserts reachable
with QT_FORCE_ASSERTS in the throttled-download drain and the request
retry) were defused as well.
