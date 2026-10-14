; owncloud.online client uninstall helper.
;
; Registered as UninstallString/QuietUninstallString of the Apps & Features
; entry by the setup wrapper (restart-wrapper.nsi). Wraps the embedded
; craft uninstaller and adds everything it does not know about:
;  1. stops a running client,
;  2. unregisters the Explorer shell extensions (overlays, context menu),
;  3. runs the embedded uninstaller silently (aborts on failure - the
;     machine keeps a working uninstall entry to retry),
;  4. removes the branded shortcuts the wrapper created,
;  5. best-effort removes the per-user navigation-pane registry entries
;     created at runtime by the client (navigationpanehelper.cpp),
;  6. removes leftover files/registry in both registry views,
;  7. reports the Windows restart that unloads the Explorer integration:
;     interactive via the finish page ("Reboot now" / "later"); silent (/S)
;     NEVER restarts on its own - exit code 3010 = restart pending (msiexec
;     convention), 0 = nothing pending; /S /forcerestart restarts right away,
;     /S /norestart is still accepted and behaves like plain /S.
;
; Everything here is limited to this client's own names (owncloud.online):
; the process owncloud.exe, "ownCloud" shortcuts and the autostart entry
; "ownCloud" belong to the official ownCloud client and are never touched.
; The embedded uninstaller and the recursive directory removal only run for
; a directory that demonstrably holds THIS client (bin\owncloud.online.exe).
;
; NOTE: this is a plain installer-mode NSIS exe, NOT an NSIS uninstaller -
; the section below must not be named "Uninstall" (that name is reserved for
; the uninstaller section and would leave this exe without any code to run).
;
; Build (see .github/workflows/main.yml):
;   makensis /DOUTFILE=<out.exe> /DVERSION=<x.y.z> admin/win/uninstall-helper.nsi

Unicode true

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"

!ifndef VERSION
  !define VERSION "0.0.0"
!endif

!define HELPER_EXE "owncloud.online-uninstall.exe"
!define CLIENT_PROCESS "owncloud.online.exe"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud"
!define REGKEY "Software\ownCloud GmbH\ownCloud"
; marker written by navigationpanehelper.cpp: ApplicationName == appNameGUI()
; == APPLICATION_NAME == "owncloud.online"
!define APPNAME_MARKER "owncloud.online"

Name "owncloud.online Client ${VERSION} Deinstallation"
OutFile "${OUTFILE}"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
BrandingText "BW-Tech GmbH - owncloud.online"
InstallDir "$PROGRAMFILES64\owncloud.online" ; placeholder - resolved in .onInit

!define MUI_ICON "..\..\src\resources\theme\universal\owncloud-online.ico"

; The finish page shows the reboot-now/-later choice because the section
; sets the reboot flag (same pattern as the install wrapper).
!define MUI_FINISHPAGE_TEXT_REBOOT "Um die Deinstallation abzuschliessen, muss Windows neu gestartet werden, damit die Explorer-Integration vollstaendig entfernt wird.$\r$\n$\r$\nTo complete the uninstallation, Windows must be restarted so that the Explorer integration is fully removed."
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_LANGUAGE "German"
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  ; This helper is installed OUTSIDE the client installation directory
  ; ($PROGRAMFILES64\owncloud.online-uninstall, see restart-wrapper.nsi): the embedded
  ; (un)installer terminates every process running from the install dir
  ; before it removes anything, and a helper inside that directory would be
  ; killed mid-run (the 7.10.2 TC-03 "nothing was removed" failure). Out
  ; here it is never in that kill zone, stays fully synchronous for silent
  ; callers, and needs no relocation tricks.
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
    ${If} ${FileExists} "$PROGRAMFILES64\owncloud.online\bin\*.*"
      StrCpy $INSTDIR "$PROGRAMFILES64\owncloud.online"
    ${ElseIf} ${FileExists} "$PROGRAMFILES64\owncloud.online\uninstall.exe"
      StrCpy $INSTDIR "$PROGRAMFILES64\owncloud.online"
    ${Else}
      StrCpy $INSTDIR "$EXEDIR"
    ${EndIf}
  ${EndIf}
  ; $9 = "1" only when the resolved directory demonstrably holds THIS client
  ; (its own binary name: bin\owncloud.online.exe since 7.4.0, or
  ; owncloud.online.exe for a flat layout). Recorded NOW, before anything
  ; gets deleted: the embedded uninstaller and the recursive directory
  ; removal are gated on it, so a stale registry value or a helper copied to
  ; e.g. the Downloads folder can never touch a foreign directory.
  ; $8 = "1" when the directory holds the OFFICIAL ownCloud client instead
  ; (upstream binary name owncloud.exe, none of our markers): then even the
  ; shared registry keys are left alone - they describe that installation.
  StrCpy $9 "0"
  ${If} ${FileExists} "$INSTDIR\bin\owncloud.online.exe"
    StrCpy $9 "1"
  ${ElseIf} ${FileExists} "$INSTDIR\owncloud.online.exe"
    StrCpy $9 "1"
  ${EndIf}
  StrCpy $8 "0"
  ${If} $9 == "0"
    ${If} ${FileExists} "$INSTDIR\bin\owncloud.exe"
      StrCpy $8 "1"
    ${ElseIf} ${FileExists} "$INSTDIR\owncloud.exe"
      StrCpy $8 "1"
    ${EndIf}
  ${EndIf}
  MessageBox MB_YESNO|MB_ICONQUESTION \
    "owncloud.online Client wirklich deinstallieren?$\r$\n($INSTDIR)$\r$\n$\r$\nReally uninstall the owncloud.online client?" \
    /SD IDYES IDYES confirmed
    Abort
  confirmed:
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
  ; Only for a directory proven to be ours ($9): the official ownCloud
  ; client ships DLLs of the same name, and unregistering those would strip
  ; its Explorer integration.
  StrCpy $6 "$WINDIR\SysNative\regsvr32.exe"
  ${IfNot} ${FileExists} "$6"
    StrCpy $6 "$SYSDIR\regsvr32.exe"
  ${EndIf}
  ${If} $9 == "1"
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
  ${EndIf}

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
  ; Only for a directory proven to be ours ($9): an uninstall.exe in a
  ; directory without our binary belongs to another product.
  ${If} $9 == "1"
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
      MessageBox MB_OK|MB_ICONEXCLAMATION \
        "Deinstallation fehlgeschlagen (Code $0). Bitte erneut versuchen.$\r$\n$\r$\nUninstallation failed (exit code $0). Please retry." /SD IDOK
      SetErrorLevel 2
      Abort "Deinstallation fehlgeschlagen / uninstallation failed (exit code $0)."
    ${EndIf}
    ; belt and braces: a "successful" exit with the client binary still in
    ; place (own binary name, either layout) means the uninstall did NOT
    ; actually happen - say so instead of wiping registry entries of a
    ; still-existing installation
    StrCpy $5 ""
    ${If} ${FileExists} "$INSTDIR\bin\owncloud.online.exe"
      StrCpy $5 "x"
    ${ElseIf} ${FileExists} "$INSTDIR\owncloud.online.exe"
      StrCpy $5 "x"
    ${EndIf}
    ${If} $5 != ""
      MessageBox MB_OK|MB_ICONEXCLAMATION \
        "Deinstallation unvollstaendig: der Client ist noch vorhanden. Bitte erneut versuchen.$\r$\n$\r$\nUninstallation incomplete: the client is still present. Please retry." /SD IDOK
      SetErrorLevel 2
      Abort "Deinstallation unvollstaendig / uninstallation incomplete."
    ${EndIf}
  ${ElseIf} $8 == "1"
    DetailPrint "$INSTDIR gehoert nicht zu owncloud.online (offizieller ownCloud-Client) - bleibt unangetastet, nur eigene Eintraege werden entfernt / belongs to the official ownCloud client - left untouched, removing own entries only."
  ${Else}
    DetailPrint "Keine owncloud.online-Installation in $INSTDIR gefunden - fahre mit Bereinigung der eigenen Eintraege fort / no owncloud.online installation found - continuing with cleanup of own entries."
  ${EndIf}

  ; ---- 4. remove the shortcuts (branded names only) ------------------------
  ; "ownCloud.lnk" shortcuts belong to the official ownCloud client and are
  ; never touched; the broken one the embedded installer creates is already
  ; removed by the setup wrapper, and the embedded uninstaller (step 3)
  ; removes its own Start menu folder.
  DetailPrint "Entferne Verknuepfungen / removing shortcuts ..."
  SetShellVarContext all
  Delete "$SMPROGRAMS\owncloud.online.lnk"
  Delete "$DESKTOP\owncloud.online.lnk"
  SetShellVarContext current
  Delete "$SMPROGRAMS\owncloud.online.lnk"
  Delete "$DESKTOP\owncloud.online.lnk"

  ; autostart entry written by the client at first run
  ; (Utility::setLaunchOnStartup -> HKCU Run, value = app name
  ; "owncloud.online" since 7.4.0; that key is shared between registry
  ; views, one delete suffices). The value "ownCloud" is the official
  ; client's and stays.
  DetailPrint "Entferne Autostart-Eintrag / removing startup entry ..."
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "owncloud.online"

  ; ---- 5. best-effort: navigation-pane entries of the elevating user -------
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

  ; ---- 6. leftover files, including this helper itself ---------------------
  ; A plain installer-mode exe cannot use the NSIS uninstaller self-copy
  ; trick: Delete /REBOOTOK on ourselves + RMDir /r /REBOOTOK schedule
  ; everything still locked (this exe, shell DLLs held by Explorer) for the
  ; next restart - which is then reported (finish page / exit code 3010).
  ${If} $9 == "1"
    Delete /REBOOTOK "$INSTDIR\uninstall.exe"
  ${EndIf}
  Delete /REBOOTOK "$INSTDIR\${HELPER_EXE}" ; stale 7.10.2-generation helper
  ; this helper itself lives in its own directory next to the installation:
  ; schedule its own removal (a
  ; running exe cannot be deleted immediately, /REBOOTOK defers it to the
  ; next restart); the non-recursive RMDir only removes an EMPTY dir, so
  ; a manually copied helper elsewhere cannot take anything else with it
  Delete /REBOOTOK "$EXEPATH"
  ${If} "$EXEDIR" != "$INSTDIR"
    RMDir /REBOOTOK "$EXEDIR"
  ${EndIf}
  ; recursive removal only when .onInit verified this is an installation
  ; of THIS client - never on a stale/foreign path
  ${If} $9 == "1"
    RMDir /r /REBOOTOK "$INSTDIR"
  ${Else}
    DetailPrint "Verzeichnis ist keine owncloud.online-Installation - wird nicht rekursiv geloescht / directory is not an owncloud.online installation - not removing recursively: $INSTDIR"
  ${EndIf}

  ; ---- 7. registry: both views, both hives (only after verified success) ---
  ; the embedded uninstaller already removed its SHCTX (32-bit view) keys;
  ; cover the 64-bit view and per-user remnants defensively. NOT when the
  ; keys describe the official craft-built ownCloud client ($8): they are
  ; its Apps & Features entry and install location, not ours.
  ${If} $8 == "1"
    DetailPrint "Registrierungseintraege beschreiben den offiziellen ownCloud-Client - bleiben erhalten / registry entries describe the official ownCloud client - kept."
  ${Else}
    DeleteRegKey HKLM "${UNINSTKEY}"
    DeleteRegKey HKLM "${REGKEY}"
    DeleteRegKey /ifempty HKLM "Software\ownCloud GmbH"
    DeleteRegKey HKCU "${UNINSTKEY}"
    DeleteRegKey HKCU "${REGKEY}"
    DeleteRegKey /ifempty HKCU "Software\ownCloud GmbH"
    SetRegView 64
    DeleteRegKey HKLM "${UNINSTKEY}"
    DeleteRegKey HKLM "${REGKEY}"
    DeleteRegKey /ifempty HKLM "Software\ownCloud GmbH"
    DeleteRegKey HKCU "${UNINSTKEY}"
    DeleteRegKey HKCU "${REGKEY}"
    DeleteRegKey /ifempty HKCU "Software\ownCloud GmbH"
    SetRegView 32
  ${EndIf}

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
