Bugfix: The client no longer asks for the password after every restart

With an account that signs in with user name and password (for example an app
password on a server with two-factor authentication), the client asked for the
password again after every restart, although the password was stored in the
keychain. At startup the folder view listed an expanded folder before the
password had been read from the keychain. That request went out without
credentials, the server answered 401, and the client took this as a rejection
of the stored password. The folder view now waits until the account is
connected, and an answer to a request that did not carry the current password
no longer counts as a rejected password. When the client does ask for the
password, it no longer waits up to a minute after it was entered before it
connects: discarding a rejected password stopped requests that were still
running until they timed out.
