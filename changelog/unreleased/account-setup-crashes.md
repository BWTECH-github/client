Bugfix: The client no longer stops right after a new account connects

Beta builds 11.0.0-beta1 to 11.0.0-beta3 stopped one to three seconds
after "Finish" in the account wizard: the activity and notification
requests of the new account were created with the account itself as
parent, which an internal check rejects. The account was never saved, so
the next start offered the setup again. The requests now belong to the
account state.

Further crashes around the account setup were fixed. An address with a
doubled scheme or "//" in its path ("https://https://server/",
"https://server//") stopped the client right after "Sign in"; the
address is now cleaned up. Addresses named by the server (the OpenID
Connect issuer from WebFinger, the instances found after signing in and
the authorization endpoint of the OpenID configuration) are cleaned up as
well, and parameters of the provider in the authorization endpoint are
kept. When a new account could not connect while the setup finished, it
was removed in the middle of a state change, which could crash; this now
happens afterwards. A renewal of the sign-in that is only delayed, for
example because the access token expired during the setup, no longer
discards the new account. "Add account..." and "Import from official
client..." bring an open wizard or dialog to the front instead of
starting a second wizard that would propose the same sync folder.
