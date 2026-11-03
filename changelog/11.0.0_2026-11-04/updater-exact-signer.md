Security: Update packages must carry our exact certificate subject

The Windows updater accepted any valid signature whose signer merely contained
"BW-TECH GmbH". It now requires the exact subject common name "BW-TECH GmbH,
IT Service für Systemintegration & Datensicherheit" and a certificate issued
by DigiCert; anything else is rejected and deleted.
