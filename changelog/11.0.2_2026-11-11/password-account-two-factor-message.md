Change: Clear message when the server requires an app password

When the server requires two-factor authentication (or app passwords in
general) for an account that signs in with a user name and password, it
rejects even the correct password. The client said "The user name or password
is not correct." and asked again and again. It now says that the server no
longer accepts the password alone and that an app password from the web
interface (Settings → Security) is needed. After a rejected password, the
client also no longer sends the rejected password again with each retry.
