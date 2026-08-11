Bugfix: Sign-in screen content stuck in the top-right corner

The sign-in step shown when the client needs re-authentication (and the
matching pages of the new-account wizard) could render its content pinned
to the top right instead of centered. Cause: alignment constants were
passed into the integer stretch parameter of addWidget/addLayout - Qt
applied them as huge stretch factors instead of alignments, letting the
content rows out-compete the centering spacers. All 29 affected call
sites across the sign-in widget and the four wizard pages now use the
correct overloads; the content is vertically centered again on every
style/DPI combination. Pure layout fix, no behavior changes.
