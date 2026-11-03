Bugfix: Error paths of chunked uploads no longer hit internal assertions

Two error paths of the resumable (chunked) upload hit an internal assertion
(fatal in builds with assertions enabled): a failed deletion of a stale chunk
while resuming an upload left the transfer outside the propagator's
bookkeeping, and a success answer other than 201 Created to the creation of
the upload folder (likewise any answer other than 207 to the resume request,
as some proxies send) was classified as a network error.

A failed deletion now starts the upload over as intended. The unexpected
answers fail the transfer with a clear error, and the next sync retries it; a
resumable upload keeps its session for that.
