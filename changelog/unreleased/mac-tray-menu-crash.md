Bugfix: The client no longer crashes when the menu bar icon is clicked on macOS 27

On macOS 27 the client could end when its menu bar menu was opened: the menu
opens with a delay there, and Qt asked the current event for its click count
even if it was not a mouse event, which AppKit answers with an exception. The two
affected queries now return 0 in that case.
