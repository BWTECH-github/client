Bugfix: An account is still recognized after its server address was changed

After "Change server address" and moving back, the account wizard no longer
recognized the account as already set up when the stored address differed
only by a trailing "/", and offered to add the same account a second time.
The wizard now compares addresses and user names the same way as the server
address change: a trailing "/" and the case of the user name do not matter.
