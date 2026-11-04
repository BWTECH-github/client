Bugfix: Server changes in folders created in the Finder reach the Mac

Files that were added or changed on the server inside a folder that had been
created on the Mac (in the Finder) never appeared there. The File Provider
extension only looked for server changes in folders that macOS had listed once,
and macOS never lists a folder it created itself. Such folders are now tracked
from the moment they are created, and a folder that already existed on the
server under the same name is compared once in full, so its files appear too.
