Bugfix: OpenID Connect settings from the system configuration are applied

The client read the OpenID Connect prompt from the ports key of the system
configuration, so a configured prompt (for example "select_account consent")
was ignored. On Linux and macOS, ports given as a list in the .ini file were
not recognised either. Both are now read correctly (fix taken over from the
upstream client).
