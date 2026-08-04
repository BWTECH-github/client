Bugfix: Bandwidth limit throttled to ~147 kB/s regardless of the setting (TC-16)

With an absolute limit configured, throughput converged to ~147 kB/s no
matter what value was set (100 kB/s ran at 147, 1000 kB/s at 149). Cause:
the limiter granted transfer quota once per second, and each grant wakes
Qt's HTTP pipeline exactly once - one wake moves at most one socket-buffer
fill (~144 KiB), so the wake rate, not the configured limit, dictated the
throughput. The quota timer now ticks ten times per second with a tenth of
the budget each, and unconsumed quota accumulates (capped at two seconds
of budget) instead of being discarded, so the configured limit is enforced
in both directions. Note: files up to 1 MiB are sent via the bulk upload
path, which is not yet throttled - the displayed transfer rate can
therefore briefly exceed the limit while small files sync.
