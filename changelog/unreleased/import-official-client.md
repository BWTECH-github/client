Enhancement: Take over accounts and folders from the official desktop client

On the first start (and via "Import from official client..." in the menu) the
client offers to take over the accounts and sync folders of the official
desktop client (owncloud.cfg of 2.x to 6.x). Its configuration and journals
are only read: every account is signed in again in the browser, classically
synchronized folders are adopted in place with a copy of their journal, so
nothing is downloaded again, and folders with virtual files are set up again
as virtual files folders (placeholders only). A running official client is
detected and reported, so that the folders are not synchronized twice. All
folders of an account are taken over; like in the folder wizard, each server
folder is synchronized only once.
