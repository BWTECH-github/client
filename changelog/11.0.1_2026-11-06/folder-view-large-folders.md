Bugfix: Expanding a folder with thousands of files no longer freezes the window

Expanding a folder with thousands of files in the folder view made the window
stop responding for minutes, on Windows it turned white. Every row was inserted
on its own and the view laid out the whole tree again after each of them. The
rows of a folder are now inserted in a few batches, the availability of virtual
files is filled in step by step without blocking the window, and a folder shows
at most 500 file rows followed by a row "… and N more files". Folder rows are
never cut, so every subfolder can still be selected for synchronization.
