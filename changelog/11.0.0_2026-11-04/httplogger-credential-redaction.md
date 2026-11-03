Security: Credentials no longer end up in HTTP debug logs

Since Qt 6.7 request header names are reported in lower case, so the HTTP
logger no longer recognised the Authorization header and wrote Basic
credentials and Bearer tokens in clear text to the debug log. Request and
response bodies of the OAuth2 token endpoint (authorization code, code
verifier, access and refresh tokens) were logged unredacted as well.

The Authorization header is now redacted regardless of its case, Cookie and
Set-Cookie headers are hidden, and token values in form and JSON bodies are
replaced by "[redacted]". Debug logs written by earlier versions may contain
credentials and should be deleted; affected app passwords and tokens should
be revoked.
