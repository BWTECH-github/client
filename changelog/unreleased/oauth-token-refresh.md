Bugfix: An expired access token no longer ends in a new sign-in

The client asked users to sign in again although their refresh token was
still valid. Three failed refresh attempts in a row (server maintenance, a
proxy error, a network change after sleep) discarded the refresh token. A
refresh gave up after 30 seconds while the server had already replaced the
tokens, and the retry with the old refresh token was rejected. A retry timer
could start a second refresh with the same refresh token, and the token
request carried the session cookie, which made the server count the client
id as a failed login and slow down further requests.

The client now always tries the refresh token first and asks for a new
sign-in only when the server rejects it (invalid_grant). Network and server
errors, timeouts, maintenance mode and unexpected answers keep the refresh
token and are retried after 30 seconds, growing to 5 minutes, while the
account shows why it is waiting. Only one refresh runs at a time, and all
requests rejected in the meantime are sent again with the new token. Tokens
are renewed before they expire (at 80 % of their lifetime and, after sleep,
before the next request). Token requests carry no cookies and wait up to
120 seconds. A new refresh token is stored in the keychain before it is
used; if that fails it stays in memory and storing it is retried. On Linux a
keychain that is not ready yet at login is read again instead of asking for
a new sign-in.

owncloudcmd with --oauth-token-file follows the same rules: it renews the
token before it expires, retries temporary failures five times and exits
with code 2 only on invalid_grant.

The oauth2 app 0.10.0 on the server accepts the previous refresh token for
another 180 seconds and answers a repeated refresh with the same new tokens,
so a refresh whose answer got lost no longer costs the sign-in either.
