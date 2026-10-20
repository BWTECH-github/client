Bugfix: The sign-in dialogs in the account view show the complete logo

When a password or a new browser sign-in was needed, the dialog in the
account view showed the logo cut off at the top and bottom, without
"ONLINE", on small screens such as 1024×768. The dialog was taller than the
account view and the layout shrank the logo first. Like the pages of the
account wizard, the content now scrolls if it does not fit, and the buttons
stay visible below it.
