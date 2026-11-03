Bugfix: Hydrating virtual files on Windows no longer crashes occasionally

The Windows virtual files support kept a pointer into the callback data of
the Cloud Filter API (the correlation vector of a hydration request) and used
it after the callback had returned, when the downloaded data was handed to
Windows, when it was validated, or when a hydration was restarted. That
memory is only valid during the callback, so the client could crash while
files were hydrated. The correlation vector is now copied.
