Enhancement: Clear setup messages when a server cannot be used

The account setup now tells apart why a server address does not work instead
of showing a technical error. If the owncloud.online server lacks the "OAuth2"
app, the wizard says so and explains how an administrator installs and enables
it; the detection checks the app's token endpoint and the OpenID Connect
configuration, so servers that sign in via OpenID Connect are not affected.
Unreachable servers, TLS and certificate problems, maintenance mode and
addresses that do not point to an owncloud.online server each get their own
message as well.
