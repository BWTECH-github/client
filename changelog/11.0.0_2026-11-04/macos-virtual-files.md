Enhancement: Virtual files in the Finder on macOS

On macOS 13 and newer the client offers virtual files through an Apple File
Provider extension, like on Windows: the account appears under "Locations"
in the Finder sidebar as "owncloud.online - user@server", files are listed as online-only placeholders and are
downloaded when opened; new, changed, renamed, moved and deleted files are
sent to the server. Changes on the server reach the Finder within about 30
seconds.

The extension gets the account's credentials from the client through the
login keychain and follows token refreshes, sign-out, pausing, removing the
account and server moves. Until the extension is switched on in System
Settings (General, Login Items & Extensions, "File Provider") the client
shows how to enable it. Classic sync with selective folders remains
available.
