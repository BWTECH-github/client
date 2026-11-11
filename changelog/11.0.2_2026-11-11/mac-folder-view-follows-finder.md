Bugfix: The folder view shows what the Finder downloads or frees on macOS

When a file was opened in the Finder, macOS downloaded it, but the folder view
of the client still showed "Available online only". The same happened after
"Remove Download" in the Finder. The client only looked again after its own
menu actions or when a folder was expanded. It now watches the folder the
Finder shows and updates the affected rows and the folders above them within
about two seconds.
