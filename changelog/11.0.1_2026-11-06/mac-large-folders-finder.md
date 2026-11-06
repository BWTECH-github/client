Bugfix: Large folders open much faster in the Finder

macOS lists a folder page by page and asked the File Provider extension for each
page separately. The extension fetched the whole folder from the server again for
every page, so opening a folder with 20,000 files took more than a quarter of an
hour with one processor core busy. The listing is now kept for the following pages.
