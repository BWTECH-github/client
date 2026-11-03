Bugfix: Cancelling a folder confirmation no longer carries out the action

"Cancel" in the confirmation for deactivating virtual files deactivated them
anyway, and all files marked as online only were downloaded: the answer of
the message box was compared with the wrong value. Quitting the client while
the confirmation to remove a folder sync connection was open removed the
connection. Both now act only on the confirming button, as do "Sync now" on
a metered connection and pausing a running sync. A right click into an empty
folder list no longer crashes the client on Windows.
