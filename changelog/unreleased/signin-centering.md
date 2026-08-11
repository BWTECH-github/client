Bugfix: Settings/sign-in window could appear in a screen corner

On the first start (and whenever the stored window placement is no longer
on any screen) the main window - including the sign-in step shown there -
appeared wherever the platform dropped it, e.g. the top-right corner of
the screen, instead of nicely centered. The window now centers itself on
its screen on first show; a placement the user chose is still restored
unchanged.

Code hygiene in the same area: 29 addWidget/addLayout calls in the
sign-in widget and the account wizard passed Qt::Alignment constants
into the int stretch parameter, where they were silently never applied
as alignments. They now use the correct overloads (provably no visual
change - the layout behavior was governed by the widgets' own alignment
and size policies all along).
