Bugfix: Pre-releases are offered the final release

A pre-release such as 11.0.0-beta1 never saw the final 11.0.0 in the update
feed, because the client compared plain version numbers and its own build
number made it look newer. The pre-release suffix now counts: a pre-release
is older than the final release of the same version, and later pre-releases
(beta2, beta10, rc1) are offered in order. Feeds of pre-releases carry their
suffix so that a pre-release never offers itself again.
