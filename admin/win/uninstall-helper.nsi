; owncloud.online client uninstall helper.
;
; Registered as UninstallString/QuietUninstallString of the Apps & Features
; entry Uninstall\owncloud.online by the setup wrapper (restart-wrapper.nsi).
; Wraps the embedded craft uninstaller and adds everything it does not know about:
;  1. stops a running client,
;  2. unregisters the Explorer shell extensions (own CLSIDs; registrations of
;     7.x under the official client's CLSIDs only where they point here),
;  3. runs the embedded uninstaller silently (aborts on failure - the
;     machine keeps a working uninstall entry to retry),
;  4. Aufräumlauf (uninstall-cleanup.ps1): meldet die Sync-Roots des Clients ab
;     (Windows entfernt die nur online verfügbaren Platzhalter, geladene Dateien
;     bleiben) und löscht auf Wunsch Sync-Datenbanken und Protokolle,
;  5. removes the shortcuts and the autostart entry,
;  6. best-effort removes the per-user navigation-pane registry entries
;     created at runtime by the client (navigationpanehelper.cpp),
;  7. Einstellungen, Konten und Anmeldedaten des angemeldeten Benutzers (E4),
;  8. removes leftover files/registry in both registry views,
;  9. reports the Windows restart that unloads the Explorer integration:
;     interactive via the finish page ("Reboot now" / "later"); silent (/S)
;     NEVER restarts on its own - exit code 3010 = restart pending (msiexec
;     convention), 0 = nothing pending; /S /forcerestart restarts right away,
;     /S /norestart is still accepted and behaves like plain /S.
;
; Entscheidung E4: Interaktiv zeigt eine Wahlseite
;   [x] Einstellungen, Konten und Anmeldedaten entfernen
;   [ ] Sync-Datenbanken und Protokolle in den Sync-Ordnern löschen
; Still behält /S Einstellungen, Konten und Anmeldedaten (z. B. für eine
; Neuinstallation per Softwareverteilung); /S /purge entfernt sie.
; Die Dateien in den Sync-Ordnern werden nie gelöscht.
;
; Everything here is limited to this client's own names (owncloud.online):
; the process owncloud.exe, "ownCloud" shortcuts, the autostart entry
; "ownCloud" and "ownCloud_*" credentials belong to the official ownCloud
; client and are never touched. The embedded uninstaller and the recursive
; directory removal only run for a directory that demonstrably holds THIS
; client (bin\owncloud.online.exe).
;
; NOTE: this is a plain installer-mode NSIS exe, NOT an NSIS uninstaller -
; the section below must not be named "Uninstall" (that name is reserved for
; the uninstaller section and would leave this exe without any code to run).
;
; Build (see .github/workflows/main.yml, run in admin/win):
;   makensis /DOUTFILE=<out.exe> /DVERSION=<x.y.z> uninstall-helper.nsi

Unicode true

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"
!include "nsDialogs.nsh"
!include /CHARSET=UTF8 "oco-common.nsh"

!ifndef VERSION
  !define VERSION "0.0.0"
!endif

; Schritt 9 verlässt sich darauf, dass DeleteRegKey /ifempty nur einen Schlüssel
; ohne Werte UND ohne Unterschlüssel löscht. So gilt das erst ab NSIS 3.06
; (Source/script.cpp: DELREGKEY_ONLYIFNOSUBKEYS|DELREGKEY_ONLYIFNOVALUES), davor
; löschte /ifempty auch einen Schlüssel mit Werten - etwa eine vom Administrator
; gesetzte Richtlinie.
!if ${NSIS_PACKEDVERSION} < 0x03006000
  !error "NSIS 3.06 or newer required: DeleteRegKey /ifempty must also check for values"
!endif

; marker written by navigationpanehelper.cpp: ApplicationName == appNameGUI()
; == APPLICATION_NAME == "owncloud.online"
!define APPNAME_MARKER "${PRODUCT}"

Name "owncloud.online ${VERSION}"
Caption "$(TXT_CAPTION)"
OutFile "${OUTFILE}"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
BrandingText "BW-Tech GmbH - owncloud.online"
InstallDir "$PROGRAMFILES64\${PRODUCT}" ; placeholder - resolved in .onInit

!define MUI_ICON "..\..\src\resources\theme\universal\owncloud-online.ico"

Var IsOurs        ; "1" = $INSTDIR enthält diesen Client (in .onInit festgestellt)
Var OptSettings   ; "1" = Einstellungen, Konten und Anmeldedaten entfernen
Var OptSyncDb     ; "1" = Sync-Datenbanken und Protokolle löschen
Var CbSettings
Var CbSyncDb

Page custom OptionsPageShow OptionsPageLeave
!define MUI_PAGE_HEADER_TEXT "$(TXT_PROGRESS_TITLE)"
!define MUI_PAGE_HEADER_SUBTEXT "$(TXT_PROGRESS_SUBTITLE)"
!define MUI_INSTFILESPAGE_FINISHHEADER_TEXT "$(TXT_DONE_TITLE)"
!define MUI_INSTFILESPAGE_FINISHHEADER_SUBTEXT "$(TXT_DONE_SUBTITLE)"
!define MUI_INSTFILESPAGE_ABORTHEADER_TEXT "$(TXT_ABORT_TITLE)"
!define MUI_INSTFILESPAGE_ABORTHEADER_SUBTEXT "$(TXT_ABORT_SUBTITLE)"
!insertmacro MUI_PAGE_INSTFILES
; The finish page shows the reboot-now/-later choice because the section
; sets the reboot flag (same pattern as the install wrapper).
!define MUI_FINISHPAGE_TITLE "$(TXT_DONE_TITLE)"
!define MUI_FINISHPAGE_TEXT "$(TXT_FINISH_TEXT)"
!define MUI_FINISHPAGE_TEXT_REBOOT "$(TXT_UN_REBOOT)"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_LANGUAGE "German"
!insertmacro MUI_LANGUAGE "English"
!define OCO_HELPER
!include /CHARSET=UTF8 "oco-texts.nsh"

Function .onInit
  ; This helper is installed OUTSIDE the client installation directory
  ; (${HELPER_DIR}, see restart-wrapper.nsi): the embedded (un)installer
  ; terminates every process running from the install dir before it removes
  ; anything, and a helper inside that directory would be killed mid-run
  ; (the 7.10.2 TC-03 "nothing was removed" failure).
  ;
  ; Resolve the install dir exactly the way the embedded uninstaller's
  ; MultiUser un.onInit will: Install_Dir from the HKLM 32-bit view (this is
  ; a 32-bit process, plain HKLM reads land in WOW6432Node). Fallbacks:
  ; HKCU (per-user install), the default location if it demonstrably holds
  ; an installation, then $EXEDIR as a last resort.
  ReadRegStr $INSTDIR HKLM "${REGKEY}" "Install_Dir"
  ${If} $INSTDIR == ""
    ReadRegStr $INSTDIR HKCU "${REGKEY}" "Install_Dir"
  ${EndIf}
  ${If} $INSTDIR == ""
    ${If} ${FileExists} "$PROGRAMFILES64\${PRODUCT}\bin\*.*"
    ${OrIf} ${FileExists} "$PROGRAMFILES64\${PRODUCT}\uninstall.exe"
      StrCpy $INSTDIR "$PROGRAMFILES64\${PRODUCT}"
    ${Else}
      StrCpy $INSTDIR "$EXEDIR"
    ${EndIf}
  ${EndIf}
  ; Recorded NOW, before anything gets deleted: the embedded uninstaller and
  ; the recursive directory removal are gated on it, so a stale registry
  ; value or a helper copied to e.g. the Downloads folder can never touch a
  ; foreign directory.
  StrCpy $R0 "$INSTDIR"
  Call OcoHasClient
  StrCpy $IsOurs $R1

  ; Vorgaben nach E4: interaktiv Einstellungen usw. entfernen (Wahlseite),
  ; still behalten - außer mit /purge
  StrCpy $OptSettings "1"
  StrCpy $OptSyncDb "0"
  ${If} ${Silent}
    StrCpy $OptSettings "0"
  ${EndIf}
  ${GetParameters} $R0
  ClearErrors
  ${GetOptions} $R0 "/purge" $R1
  ${IfNot} ${Errors}
    StrCpy $OptSettings "1"
  ${EndIf}
FunctionEnd

; Wahlseite (nur interaktiv; still werden Seiten übersprungen)
Function OptionsPageShow
  !insertmacro MUI_HEADER_TEXT "$(TXT_OPTIONS_TITLE)" "$(TXT_OPTIONS_SUBTITLE)"
  nsDialogs::Create 1018
  Pop $R0
  ${If} $R0 == error
    Abort
  ${EndIf}
  ${NSD_CreateLabel} 0 0 100% 52u "$(TXT_OPTIONS_INTRO)"
  Pop $R0
  ${NSD_CreateCheckbox} 0 56u 100% 12u "$(TXT_OPT_SETTINGS)"
  Pop $CbSettings
  ${If} $OptSettings == "1"
    ${NSD_Check} $CbSettings
  ${EndIf}
  ${NSD_CreateLabel} 12u 69u -12u 18u "$(TXT_OPT_SETTINGS_HINT)"
  Pop $R0
  ${NSD_CreateCheckbox} 0 92u 100% 12u "$(TXT_OPT_SYNCDB)"
  Pop $CbSyncDb
  ${If} $OptSyncDb == "1"
    ${NSD_Check} $CbSyncDb
  ${EndIf}
  ${NSD_CreateLabel} 12u 105u -12u 18u "$(TXT_OPT_SYNCDB_HINT)"
  Pop $R0
  ; die Schaltfläche, die die Deinstallation startet, heißt auch so
  GetDlgItem $R0 $HWNDPARENT 1
  SendMessage $R0 ${WM_SETTEXT} 0 "STR:$(TXT_UNINSTALL_BUTTON)"
  nsDialogs::Show
FunctionEnd

Function OptionsPageLeave
  ${NSD_GetState} $CbSettings $R0
  ${If} $R0 == ${BST_CHECKED}
    StrCpy $OptSettings "1"
  ${Else}
    StrCpy $OptSettings "0"
  ${EndIf}
  ${NSD_GetState} $CbSyncDb $R0
  ${If} $R0 == ${BST_CHECKED}
    StrCpy $OptSyncDb "1"
  ${Else}
    StrCpy $OptSyncDb "0"
  ${EndIf}
FunctionEnd

; Aufräumlauf als 64-Bit-PowerShell (Beschreibung in uninstall-cleanup.ps1).
; Das Skript läuft als ScriptBlock über -Command - eine Ausführungsrichtlinie
; für Skriptdateien greift so nicht; sein Pfad kommt über die Umgebung, damit
; Sonderzeichen im Temp-Pfad (Apostroph im Benutzernamen) nichts brechen.
; Nach zehn Minuten bricht nsExec ab; das Skript löscht die Sync-Datenbank vor
; dem Abmelden, ein Abbruch hinterlässt also keine Datenbank, die entfernte
; Platzhalter noch kennt.
Function OcoRunCleanup
  InitPluginsDir
  File "/oname=$PLUGINSDIR\uninstall-cleanup.ps1" "uninstall-cleanup.ps1"
  StrCpy $R0 "$WINDIR\SysNative\WindowsPowerShell\v1.0\powershell.exe"
  ${IfNot} ${FileExists} "$R0"
    StrCpy $R0 "$SYSDIR\WindowsPowerShell\v1.0\powershell.exe"
  ${EndIf}
  ${IfNot} ${FileExists} "$R0"
    DetailPrint "PowerShell nicht gefunden - Sync-Roots bleiben angemeldet / PowerShell not found - sync roots stay registered."
    Return
  ${EndIf}
  System::Call 'kernel32::SetEnvironmentVariable(t "OCO_CLEANUP_SCRIPT", t "$PLUGINSDIR\uninstall-cleanup.ps1") i'
  StrCpy $R1 ""
  ${If} $OptSyncDb == "1"
    StrCpy $R1 " -SyncDb"
  ${EndIf}
  DetailPrint "Melde Sync-Roots ab / unregistering sync roots ..."
  nsExec::ExecToLog /TIMEOUT=600000 '"$R0" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "& ([ScriptBlock]::Create([IO.File]::ReadAllText($$env:OCO_CLEANUP_SCRIPT)))$R1"'
  Pop $R2
  DetailPrint "Aufraeumlauf beendet / clean-up finished: $R2"
FunctionEnd

; Anmeldedaten des Clients in der Anmeldeinformationsverwaltung des
; angemeldeten Benutzers: generische Einträge "owncloud.online_credentials:..."
; (QtKeychain, CredentialManager::scopedKey). Einträge des offiziellen
; Clients ("ownCloud_credentials:...") passen nicht auf den Filter.
; 32-Bit-Prozess: Zeiger sind 4 Byte groß.
Function OcoDeleteCredentials
  System::Call 'advapi32::CredEnumerateW(w "${PRODUCT}_*", i 0, *i .R1, *i .R2) i .R0'
  ${If} $R0 == 0
    Return
  ${EndIf}
  StrCpy $R3 0
  ${DoWhile} $R3 < $R1
    IntOp $R4 $R3 * 4
    IntOp $R4 $R4 + $R2
    System::Call '*$R4(i .R5)'
    ; CREDENTIALW: Flags, Type, TargetName
    System::Call '*$R5(i, i .R6, w .R0)'
    System::Call 'advapi32::CredDeleteW(w R0, i R6, i 0) i .R5'
    DetailPrint "  Anmeldedaten / credential: $R0 ($R5)"
    IntOp $R3 $R3 + 1
  ${Loop}
  System::Call 'advapi32::CredFree(i R2)'
FunctionEnd

Section "-remove"
  SetDetailsPrint both
  ; release any working-directory handle on the install dir
  SetOutPath "$TEMP"

  ; ---- 1. stop a running client (own process name only) --------------------
  ; owncloud.exe is the official ownCloud client and is never stopped here
  ; (our builds have been named owncloud.online.exe since 7.4.0)
  DetailPrint "Beende laufenden owncloud.online Client / stopping running client ..."
  nsExec::ExecToLog 'taskkill /F /IM "${CLIENT_PROCESS}"'
  Pop $0
  Sleep 1500

  ; ---- 2. unregister the shell extensions (while the DLLs still exist) -----
  ; 64-bit DLLs -> 64-bit regsvr32: from this 32-bit process that is
  ; $WINDIR\SysNative; fall back to $SYSDIR (same pattern as the wrapper).
  ; Only for a directory proven to be ours. Die DLLs tragen eigene Kennungen;
  ; Registrierungen aus 7.x unter den Kennungen des offiziellen Clients werden
  ; nur entfernt, wenn sie hierher zeigen (oco-common.nsh).
  StrCpy $6 "$WINDIR\SysNative\regsvr32.exe"
  ${IfNot} ${FileExists} "$6"
    StrCpy $6 "$SYSDIR\regsvr32.exe"
  ${EndIf}
  ${If} $IsOurs == "1"
    ${If} ${FileExists} "$INSTDIR\bin\OCOverlays.dll"
      DetailPrint "Deregistriere Overlay-Symbole / unregistering overlay icons ..."
      nsExec::ExecToLog '"$6" /u /s "$INSTDIR\bin\OCOverlays.dll"'
      Pop $0
    ${EndIf}
    ${If} ${FileExists} "$INSTDIR\bin\OCContextMenu.dll"
      DetailPrint "Deregistriere Kontextmenue / unregistering context menu ..."
      nsExec::ExecToLog '"$6" /u /s "$INSTDIR\bin\OCContextMenu.dll"'
      Pop $0
    ${EndIf}
    StrCpy $R0 "$INSTDIR"
    Call OcoDropLegacyShellExt
  ${EndIf}
  ; Altschlüssel aus 7.x, die noch auf diesen Client zeigen (z. B. auf diesen
  ; Helfer) - solange die Installation noch auf der Platte liegt
  Call OcoDropLegacyKeys

  ; ---- 3. run the embedded uninstaller (as a %TEMP% copy) ------------------
  ; NEVER run it in-place with _?=: its process check (EndProcessWithDialog)
  ; lists every process running from $INSTDIR - in-place that includes the
  ; uninstaller ITSELF - and the silent default answer "Yes" kills them all
  ; before anything is removed (the TC-03 "nothing was deleted" failure).
  ; A copy in %TEMP% started with _?=$INSTDIR runs outside that kill zone,
  ; ExecWait really waits and the exit code is real. Its MultiUser un.onInit
  ; re-reads Install_Dir (same value .onInit resolved), so the target agrees.
  ; On failure: ABORT without touching any registry, so Apps & Features
  ; keeps a working entry to retry.
  ; Only for a directory proven to be ours: an uninstall.exe in a directory
  ; without our binary belongs to another product.
  ${If} $IsOurs == "1"
  ${AndIf} ${FileExists} "$INSTDIR\uninstall.exe"
    DetailPrint "Starte eingebetteten Uninstaller / running embedded uninstaller ..."
    GetTempFileName $R3
    Delete "$R3"
    StrCpy $R3 "$R3.exe"
    ClearErrors
    CopyFiles /SILENT "$INSTDIR\uninstall.exe" "$R3"
    ${If} ${Errors}
    ${OrIfNot} ${FileExists} "$R3"
      ; copy failed - run in place as a last resort (pre-7.10.3 behavior)
      StrCpy $R3 "$INSTDIR\uninstall.exe"
    ${EndIf}
    ClearErrors
    ExecWait '"$R3" /S _?=$INSTDIR' $0
    ; ExecWait leaves $0 untouched when the process could not be started at
    ; all (AV block, corrupt exe) - treat that as failure, not success
    IfErrors 0 +2
      StrCpy $0 "-1"
    DetailPrint "Uninstaller exit code: $0"
    ${If} "$R3" != "$INSTDIR\uninstall.exe"
      Delete "$R3"
    ${EndIf}
    ${If} $0 != 0
      MessageBox MB_OK|MB_ICONEXCLAMATION "$(TXT_UN_FAILED)" /SD IDOK
      SetErrorLevel 2
      Abort "$(TXT_UN_FAILED)"
    ${EndIf}
    ; belt and braces: a "successful" exit with the client binary still in
    ; place (own binary name, either layout) means the uninstall did NOT
    ; actually happen - say so instead of wiping registry entries of a
    ; still-existing installation
    StrCpy $R0 "$INSTDIR"
    Call OcoHasClient
    ${If} $R1 == "1"
      MessageBox MB_OK|MB_ICONEXCLAMATION "$(TXT_UN_INCOMPLETE)" /SD IDOK
      SetErrorLevel 2
      Abort "$(TXT_UN_INCOMPLETE)"
    ${EndIf}
  ${Else}
    DetailPrint "Keine owncloud.online-Installation in $INSTDIR gefunden - fahre mit Bereinigung der eigenen Eintraege fort / no owncloud.online installation found - continuing with cleanup of own entries."
  ${EndIf}

  ; ---- 4. Sync-Roots abmelden, auf Wunsch Sync-Datenbanken löschen ---------
  Call OcoRunCleanup

  ; ---- 5. remove the shortcuts (branded names only) ------------------------
  ; The embedded uninstaller (step 3) removes its Start menu folder
  ; "owncloud.online"; 7.x wrappers placed the shortcut directly in the Start
  ; menu. "ownCloud.lnk" shortcuts belong to the official ownCloud client and
  ; are never touched.
  DetailPrint "Entferne Verknuepfungen / removing shortcuts ..."
  SetShellVarContext all
  Delete "$SMPROGRAMS\${PRODUCT}.lnk"
  Delete "$DESKTOP\${PRODUCT}.lnk"
  SetShellVarContext current
  Delete "$SMPROGRAMS\${PRODUCT}.lnk"
  Delete "$DESKTOP\${PRODUCT}.lnk"

  ; autostart entry written by the client at first run
  ; (Utility::setLaunchOnStartup -> HKCU Run, value = app name
  ; "owncloud.online" since 7.4.0; that key is shared between registry
  ; views, one delete suffices). The value "ownCloud" is the official
  ; client's and stays.
  DetailPrint "Entferne Autostart-Eintrag / removing startup entry ..."
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "${PRODUCT}"

  ; ---- 6. best-effort: navigation-pane entries of the elevating user -------
  ; navigationpanehelper.cpp writes them from the 64-bit client, so
  ; HKCU\Software\Classes\CLSID entries live in the 64-bit view. Recognize
  ; ours by the ApplicationName marker; do not increment the index after a
  ; delete (enumeration shifts).
  DetailPrint "Entferne Navigationsbereich-Eintraege / removing navigation pane entries ..."
  SetRegView 64
  StrCpy $1 0
  navLoop:
    EnumRegKey $2 HKCU "Software\Classes\CLSID" $1
    StrCmp $2 "" navDone
    ReadRegStr $3 HKCU "Software\Classes\CLSID\$2" "ApplicationName"
    ${If} $3 == "${APPNAME_MARKER}"
      DetailPrint "  $2"
      DeleteRegKey HKCU "Software\Classes\CLSID\$2"
      DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\$2"
      DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Explorer\HideDesktopIcons\NewStartPanel" "$2"
      ; if the delete did not take effect (restrictive ACL), advance the
      ; index anyway so the loop cannot spin on the same key forever
      ReadRegStr $4 HKCU "Software\Classes\CLSID\$2" "ApplicationName"
      ${If} $4 == "${APPNAME_MARKER}"
        IntOp $1 $1 + 1
      ${EndIf}
    ${Else}
      IntOp $1 $1 + 1
    ${EndIf}
    Goto navLoop
  navDone:
  SetRegView 32

  ; ---- 7. Einstellungen, Konten und Anmeldedaten (E4) ----------------------
  ; Profil des Benutzers, unter dem der Helfer läuft (bei UAC derselbe
  ; Benutzer; unter SYSTEM, z. B. Softwareverteilung mit /purge, nur dessen
  ; Profil - andere Profile bleiben unberührt).
  ${If} $OptSettings == "1"
    DetailPrint "Entferne Einstellungen, Konten und Anmeldedaten / removing settings, accounts and credentials ..."
    SetShellVarContext current
    ; leerer Profilpfad ergäbe "\owncloud.online" im Laufwerksstamm
    ${If} $APPDATA != ""
      RMDir /r "$APPDATA\${PRODUCT}"
    ${EndIf}
    ${If} $LOCALAPPDATA != ""
      RMDir /r "$LOCALAPPDATA\${PRODUCT}"
    ${EndIf}
    Call OcoDeleteCredentials
  ${Else}
    DetailPrint "Einstellungen, Konten und Anmeldedaten bleiben erhalten / settings, accounts and credentials are kept."
  ${EndIf}

  ; ---- 8. leftover files, including this helper itself ---------------------
  ; A plain installer-mode exe cannot use the NSIS uninstaller self-copy
  ; trick: Delete /REBOOTOK on ourselves + RMDir /r /REBOOTOK schedule
  ; everything still locked (this exe, shell DLLs held by Explorer) for the
  ; next restart - which is then reported (finish page / exit code 3010).
  ${If} $IsOurs == "1"
    Delete /REBOOTOK "$INSTDIR\uninstall.exe"
  ${EndIf}
  Delete /REBOOTOK "$INSTDIR\${HELPER_EXE}" ; stale 7.10.2-generation helper
  ; this helper itself lives in its own directory next to the installation:
  ; schedule its own removal (a running exe cannot be deleted immediately,
  ; /REBOOTOK defers it to the next restart); the non-recursive RMDir only
  ; removes an EMPTY dir, so a manually copied helper elsewhere cannot take
  ; anything else with it
  Delete /REBOOTOK "$EXEPATH"
  ${If} "$EXEDIR" != "$INSTDIR"
    RMDir /REBOOTOK "$EXEDIR"
  ${EndIf}
  ; recursive removal only when .onInit verified this is an installation
  ; of THIS client - never on a stale/foreign path
  ${If} $IsOurs == "1"
    RMDir /r /REBOOTOK "$INSTDIR"
  ${Else}
    DetailPrint "Verzeichnis ist keine owncloud.online-Installation - wird nicht rekursiv geloescht / directory is not an owncloud.online installation - not removing recursively: $INSTDIR"
  ${EndIf}

  ; ---- 9. registry: own keys, both views, both hives -----------------------
  ; the embedded uninstaller already removed its SHCTX (32-bit view) keys;
  ; cover the 64-bit view and per-user remnants defensively. Seit der eigenen
  ; Identität teilen diese Schlüssel nichts mehr mit dem offiziellen Client.
  DeleteRegKey HKLM "${UNINSTKEY}"
  DeleteRegKey HKLM "${REGKEY}"
  DeleteRegKey /ifempty HKLM "${VENDORKEY}"
  DeleteRegKey HKCU "${UNINSTKEY}"
  DeleteRegKey HKCU "${REGKEY}"
  DeleteRegKey /ifempty HKCU "${VENDORKEY}"
  SetRegView 64
  DeleteRegKey HKLM "${UNINSTKEY}"
  DeleteRegKey HKLM "${REGKEY}"
  DeleteRegKey /ifempty HKLM "${VENDORKEY}"
  DeleteRegKey HKCU "${UNINSTKEY}"
  DeleteRegKey HKCU "${REGKEY}"
  DeleteRegKey /ifempty HKCU "${VENDORKEY}"
  ; Leerer Richtlinien-Schlüssel: Mit Administratorrechten gestartet, legte der
  ; Client (7.4 bis 11.0.0-beta2) ihn beim Lesen der Richtlinien an (QSettings
  ; erzeugt den Schlüssel beim Öffnen). Nur ohne Werte und ohne Unterschlüssel,
  ; ebenso danach der Herstellerschlüssel: Vom Administrator gesetzte
  ; Richtlinien (GPO) bleiben, auch die anderer Produkte. HKLM\Software\Policies
  ; ist in beiden Registry-Ansichten derselbe Schlüssel.
  DeleteRegKey /ifempty HKLM "${POLICYKEY}"
  DeleteRegKey /ifempty HKLM "${POLICY_VENDORKEY}"
  SetRegView 32

  ; Interactive: keep offering the Windows restart on the finish page (the
  ; Explorer integration unloads reliably only after one). Silent: leave the
  ; flag to NSIS - it is set only when a file had to be scheduled for the
  ; next restart via /REBOOTOK (this helper itself always is, shell DLLs
  ; held by Explorer usually are) - so the exit code below reports a pending
  ; restart only when there really is one.
  ${IfNot} ${Silent}
    SetRebootFlag true
  ${EndIf}
SectionEnd

Function .onInstSuccess
  ; Silent mode has no finish page. The helper never restarts Windows on its
  ; own: deployment tools (baramundi, Intune, SCCM) manage reboots themselves
  ; and expect the msiexec convention - exit code 3010 = "removed, restart
  ; pending", 0 = nothing pending.
  ;   /S                -> no restart; 3010 if a restart is pending, else 0
  ;   /S /norestart     -> same (kept for existing deployment jobs)
  ;   /S /forcerestart  -> restarts Windows right away (optional)
  ; (Identical pattern to restart-wrapper.nsi.)
  ${IfNot} ${Silent}
    Return
  ${EndIf}
  ${GetParameters} $R0
  ClearErrors
  ${GetOptions} $R0 "/forcerestart" $R1
  ${IfNot} ${Errors}
    DetailPrint "Starte Windows neu (/forcerestart) / restarting Windows ..."
    Reboot
    ; only reached when the restart could not be initiated
    SetErrorLevel 3010
    Return
  ${EndIf}
  ${If} ${RebootFlag}
    DetailPrint "Neustart ausstehend, Exit-Code 3010 / restart pending, exit code 3010."
    SetErrorLevel 3010
  ${EndIf}
FunctionEnd
