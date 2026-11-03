Enhancement: Sign in with user name and password on servers without OAuth2

Like the 6.0.3 client, the desktop client again signs in with user name and
password (or an app password, which is recommended) when the server has
neither the "OAuth2" app nor OpenID Connect. After the server address, the
wizard detects how the server wants users to sign in: servers with OAuth2 or
OpenID Connect still open the browser as before, all others show a page for
user name and password instead of the message about the missing "OAuth2" app.
A wrong password is reported clearly and can be corrected on the spot.

The password is kept in the system keychain. When the server rejects it later,
the account view asks for the password again ("Stay logged out" signs the
account out); signing out removes it from the keychain. Changing the server
address and taking over accounts from the official client work for these
accounts as well; the login name of the official client is filled in. The
account settings store the sign-in method with the keys of the 6.0.3 client
(http_oauth, http_user) and no longer remove them when loading an older
configuration. An account whose keys an earlier 11.0 beta already removed is
treated as an OAuth2 account; remove it and set it up again.

The password never appears in logs: the HTTP log redacts the Authorization
header ("Basic [redacted]") and password form fields.
