Bugfix: No spaces request to servers without spaces

Right after sign-in the client could ask the server for graph/v1.0/me/drives
before it knew the server capabilities, which a server without spaces such as
owncloud.online answers with an error. The spaces list is now only requested
once the capabilities announce spaces.
