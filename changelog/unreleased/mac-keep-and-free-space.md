Bugfix: Keep files on the Mac or free their space, in the client and in the Finder

On macOS the menu entries "Always keep on this device", "Free up space (online
only)" and "Reset to default" in the folder view of the client had no effect:
the choice was only stored in the client's database. It now reaches the File
Provider extension. macOS downloads the file or folder including everything in
it and keeps it up to date, or frees its space. A choice for a folder applies to
everything in it and replaces choices made for items inside. The Finder now offers "Remove
Download" for downloaded items, which was missing because the extension did not
declare items as evictable. The folder view of the client shows the real state
of each item instead of nothing.
