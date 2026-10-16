Enhancement: Take over accounts and folders from the official ownCloud client

On the first start (and via "Import from official client..." in the menu) the
client offers to take over the accounts and sync folders of the official
ownCloud desktop client (owncloud.cfg of 2.x to 6.x). Its configuration and
journals are only read: every account is signed in again in the browser,
classically synchronized folders are adopted in place with a copy of their
journal, so nothing is downloaded again, and folders with virtual files are
set up again as virtual files folders (placeholders only). A running official
client is detected and reported, so that the folders are not synchronized
twice. For now one folder per account can be taken over.
