; owncloud.online client setup wrapper.
;
; Wraps the craft-built NSIS installer. Seit dem Blueprint-Fork (BWTECH-github/
; craft-blueprints-owncloud, Zweig bwtech) trägt dieser die eigene Identität:
; Programme und Features "owncloud.online" / "BW-Tech GmbH" mit eigenem Link,
; Startmenü-Ordner "owncloud.online" mit funktionierender Verknüpfung,
; Schlüssel Software\BW-Tech GmbH\owncloud.online und Uninstall\owncloud.online.
; Der Wrapper ergänzt nur noch, was eine verwaltete Verteilung braucht:
;  1. stops a running client first, so the installer can actually replace the
;     binaries (a running instance otherwise keeps the old version alive),
;  2. runs the embedded installer silently,
;  3. registers the Explorer integration (overlay icons, context menu), leitet
;     Programme und Features über den Deinstallations-Helfer (Explorer-
;     Erweiterungen abmelden, Aufräumlauf, Wahlseite nach E4) und legt die
;     Desktop-Verknüpfung an,
;  4. migriert Installationen von 7.x (upstream-Identität "ownCloud", die der
;     offizielle Client teilt) und entfernt deren Altschlüssel,
;  5. reports whether a Windows restart is pending - without ever forcing it.
;
; Interactive: the finish page offers "Reboot now" (default) / "reboot later"
; for the restart that loads the Explorer integration.
; Silent (/S, e.g. baramundi): never restarts Windows on its own. Exit code
; 3010 means "installed, restart pending" (msiexec convention), 0 means
; nothing is pending. /S /forcerestart restarts right away; /S /norestart is
; still accepted and behaves like plain /S.
;
; The old default directory $PROGRAMFILES64\ownCloud is also the home of the
; official ownCloud client. It is only touched when it demonstrably holds an
; old installation of THIS client (bin\owncloud.online.exe, versions
; 7.4 - 7.7). Nothing that belongs to the official client - the process
; owncloud.exe, "ownCloud" shortcuts, the autostart entry "ownCloud", its
; Explorer registrations and registry keys - is stopped, deleted or changed
; by this setup.
;
; Build (see .github/workflows/main.yml, run in admin/win):
;   makensis /DSOURCE_EXE=<craft-installer.exe> /DOUTFILE=<out.exe> \
;            /DVERSION=<x.y.z> /DUNINSTALL_HELPER_EXE=<helper.exe> \
;            restart-wrapper.nsi

Unicode true

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"
!include /CHARSET=UTF8 "oco-common.nsh"

!ifndef VERSION
  !define VERSION "0.0.0"
!endif

; Compiled uninstall helper (admin/win/uninstall-helper.nsi), built first by
; CI and embedded here; it becomes the Apps & Features uninstall entry so
; shortcuts, shell-extension registration and the restart requirement are
; handled on uninstall too.
!ifndef UNINSTALL_HELPER_EXE
  !error "UNINSTALL_HELPER_EXE must point at the compiled uninstall helper (see main.yml)"
!endif

Name "owncloud.online ${VERSION}"
OutFile "${OUTFILE}"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
BrandingText "BW-Tech GmbH - owncloud.online"

; Branded default install directory. A different path can be chosen on the
; directory page, or passed as /D=... (must be the last parameter, unquoted) -
; it is forwarded to the embedded installer. Ohne /D= übernimmt .onInit den
; Ordner einer vorhandenen Installation. An old installation of THIS client
; in the previous default directory ($PROGRAMFILES64\ownCloud, used by
; 7.4 - 7.7) is removed after the new install has been verified, so upgrades
; move over cleanly (accounts and sync folders live in the user profile and
; are not touched by that).
InstallDir "$PROGRAMFILES64\${PRODUCT}"
InstallDirRegKey HKLM "${REGKEY}" "Install_Dir"
!define OLD_INSTDIR "$PROGRAMFILES64\ownCloud"
; That directory is also the default of the official ownCloud client (MSI
; 6.x: owncloud.exe in the root, craft 7.x: bin\owncloud.exe). Only this
; marker proves that it holds an old installation of THIS client: our builds
; ship bin\owncloud.online.exe since 7.4.0, no other product does. Without
; the marker the directory is left exactly as it is.
!define OLD_INSTDIR_MARKER "${OLD_INSTDIR}\bin\${CLIENT_PROCESS}"

!define MUI_ICON "..\..\src\resources\theme\universal\owncloud-online.ico"

; The finish page shows the reboot-now/-later choice whenever the reboot flag
; is set: in interactive mode the install section sets it (the Explorer
; integration loads reliably only after a restart); in silent mode only NSIS
; itself sets it, when a locked file had to be scheduled via /REBOOTOK.
!define MUI_FINISHPAGE_TEXT_REBOOT "$(TXT_REBOOT)"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_LANGUAGE "German"
!insertmacro MUI_LANGUAGE "English"
!define OCO_WRAPPER
!include /CHARSET=UTF8 "oco-texts.nsh"

Var ClientExe
; beiseitegelegte Explorer-Erweiterungen der vorhandenen Installation
Var OldOverlayDll
Var OldMenuDll

; in: $2 = candidate install dir. out: $ClientExe = full path of the client
; binary, or "" if none found. Only this client's own binary name counts:
; bin\owncloud.online.exe (since 7.4.0) or owncloud.online.exe (flat
; layout). The upstream name owncloud.exe is the official ownCloud client
; and is never adopted, registered or linked to.
Function FindClientExe
  ${If} ${FileExists} "$2\bin\${CLIENT_PROCESS}"
    StrCpy $ClientExe "$2\bin\${CLIENT_PROCESS}"
  ${ElseIf} ${FileExists} "$2\${CLIENT_PROCESS}"
    StrCpy $ClientExe "$2\${CLIENT_PROCESS}"
  ${Else}
    StrCpy $ClientExe ""
  ${EndIf}
FunctionEnd

; Explorer hält die Erweiterungs-DLLs einer vorhandenen Installation geladen;
; der eingebettete Installer (7za) kann sie dann nicht überschreiben, und die
; neue Version käme nie an. Eine geladene DLL lässt sich umbenennen: vor der
; Installation beiseitelegen, danach löschen (gesperrt: beim Neustart), bei
; einem Fehlschlag zurückbenennen.
!macro OcoMoveAside _name _var
  StrCpy ${_var} ""
  ${If} ${FileExists} "$INSTDIR\bin\${_name}"
    System::Call 'kernel32::GetTickCount() i .R0'
    ClearErrors
    Rename "$INSTDIR\bin\${_name}" "$INSTDIR\bin\${_name}.$R0.old"
    ${IfNot} ${Errors}
      StrCpy ${_var} "$INSTDIR\bin\${_name}.$R0.old"
    ${EndIf}
  ${EndIf}
!macroend

!macro OcoMoveBack _name _var
  ${If} ${_var} != ""
  ${AndIfNot} ${FileExists} "$INSTDIR\bin\${_name}"
    Rename "${_var}" "$INSTDIR\bin\${_name}"
  ${EndIf}
!macroend

Function .onInit
  ; NSIS hat $INSTDIR schon gesetzt: /D= (NSIS entfernt es aus der
  ; Befehlszeile) und Install_Dir der eigenen Identität (InstallDirRegKey)
  ; gehen vor. Steht noch der Standardordner da, den Ordner einer
  ; 7.x-Installation übernehmen - nur mit diesem Client darin; der alte
  ; Standardordner ${OLD_INSTDIR} wird stattdessen migriert (siehe Abschnitt).
  ${If} $INSTDIR != "$PROGRAMFILES64\${PRODUCT}"
    Return
  ${EndIf}
  ReadRegStr $R0 HKLM "${LEGACY_REGKEY}" "Install_Dir"
  ${If} $R0 != ""
  ${AndIf} $R0 != "${OLD_INSTDIR}"
    Call OcoHasClient
    ${If} $R1 == "1"
      StrCpy $INSTDIR $R0
    ${EndIf}
  ${EndIf}
FunctionEnd

Section "Install"
  SetDetailsPrint both

  ; Stop a running client; ignore errors (it may simply not be running).
  ; Only our own process name: owncloud.exe is the official ownCloud client
  ; and is never stopped by this setup (our builds have been named
  ; owncloud.online.exe since 7.4.0).
  DetailPrint "Beende laufenden owncloud.online Client / stopping running client ..."
  nsExec::ExecToLog 'taskkill /F /IM "${CLIENT_PROCESS}"'
  Pop $0
  Sleep 1500

  ; ---- Steer the embedded installer to $INSTDIR -----------------------------
  ; The embedded craft installer IGNORES /D= completely: its MultiUser setup
  ; unconditionally resets $INSTDIR in .onInit and then applies the Install_Dir
  ; value from HKLM "${REGKEY}" (32-bit view) when present. Writing that value
  ; beforehand is the only reliable way to choose its target directory - it is
  ; the same mechanism its own upgrades use. (This wrapper is a 32-bit
  ; process, so a plain HKLM write lands in the same WOW6432Node view the
  ; embedded installer reads.)
  ; $8 remembers the previous value so a failed install can restore it.
  ReadRegStr $8 HKLM "${REGKEY}" "Install_Dir"
  WriteRegStr HKLM "${REGKEY}" "Install_Dir" "$INSTDIR"

  ; ---- Vorhandene Installation dieses Clients im Zielordner vorbereiten -----
  ; 7.x registrierte die Explorer-Erweiterungen unter den Kennungen des
  ; offiziellen Clients, die neuen DLLs bringen eigene mit: alte Einträge, die
  ; hierher zeigen, entfernen (sonst zeigten sie auf DLLs ohne diese Klassen);
  ; gesperrte DLLs beiseitelegen, damit die neuen ankommen.
  StrCpy $OldOverlayDll ""
  StrCpy $OldMenuDll ""
  StrCpy $R0 "$INSTDIR"
  Call OcoHasClient
  ${If} $R1 == "1"
    Call OcoDropLegacyShellExt
    !insertmacro OcoMoveAside "OCOverlays.dll" $OldOverlayDll
    !insertmacro OcoMoveAside "OCContextMenu.dll" $OldMenuDll
  ${EndIf}

  ; Unpack and run the real installer silently.
  InitPluginsDir
  SetOutPath "$PLUGINSDIR"
  File "/oname=client-setup.exe" "${SOURCE_EXE}"
  DetailPrint "Installiere owncloud.online Client ${VERSION} nach $INSTDIR / installing ..."
  ; /D= is kept as belt and braces although the registry value above decides
  ExecWait '"$PLUGINSDIR\client-setup.exe" /S /D=$INSTDIR' $0
  DetailPrint "Installer exit code: $0"
  IntCmp $0 0 installOk
    SetDetailsPrint both
    DetailPrint "Installation fehlgeschlagen / installation failed (exit code $0)."
    ; restore the previous steering value - whatever installation the machine
    ; really has still lives where it did before this attempt
    ${If} $8 != ""
      WriteRegStr HKLM "${REGKEY}" "Install_Dir" "$8"
    ${Else}
      DeleteRegValue HKLM "${REGKEY}" "Install_Dir"
    ${EndIf}
    !insertmacro OcoMoveBack "OCOverlays.dll" $OldOverlayDll
    !insertmacro OcoMoveBack "OCContextMenu.dll" $OldMenuDll
    MessageBox MB_OK|MB_ICONEXCLAMATION "$(TXT_FAILED)" /SD IDOK
    Abort "$(TXT_FAILED)"
  installOk:
  ${If} $OldOverlayDll != ""
    Delete /REBOOTOK "$OldOverlayDll"
  ${EndIf}
  ${If} $OldMenuDll != ""
    Delete /REBOOTOK "$OldMenuDll"
  ${EndIf}

  ; ---- Verify where the client actually landed ------------------------------
  ; $9 = client binary strictly under $INSTDIR, or "" - the migration below
  ; must only ever run when the new client verifiably lives OUTSIDE the old
  ; directory, so it can never delete the installation we just made.
  StrCpy $2 "$INSTDIR"
  Call FindClientExe
  StrCpy $9 "$ClientExe"

  ; ---- Install the uninstall helper (OUTSIDE the install dir) ---------------
  ; The embedded (un)installer kills every process running from the install
  ; directory before removing anything - an uninstall helper inside it would
  ; be terminated mid-run. It lives in its own directory NEXT TO (not
  ; inside) the installation: outside the kill zone (which is exactly
  ; $INSTDIR\*), and admin-only-writable - a helper in a user-writable
  ; location like ProgramData could be replaced by any local user and would
  ; then run elevated.
  DetailPrint "Installiere Deinstallations-Helfer / installing uninstall helper ..."
  CreateDirectory "${HELPER_DIR}"
  SetOutPath "${HELPER_DIR}"
  File "/oname=${HELPER_EXE}" "${UNINSTALL_HELPER_EXE}"
  ; remove the misplaced 7.10.2-generation helper from the install dir
  Delete "$INSTDIR\${HELPER_EXE}"

  ; ---- Migrate installations of 7.x -----------------------------------------
  ${If} $9 != ""
    ; Altschlüssel Uninstall\ownCloud und Software\ownCloud GmbH\ownCloud, wenn
    ; sie zu diesem Client gehören: Der Eintrag "ownCloud" doppelte sonst den
    ; neuen in Programme und Features, und das MSI des offiziellen Clients
    ; startete seinen UninstallString als vermeintliche Altinstallation
    ; (Install-Probe K6). Vor dem Entfernen des alten Ordners: die Zuordnung
    ; prüft, ob er diesen Client enthält.
    Call OcoDropLegacyKeys
    ; 7.x legte die Verknüpfung direkt ins Startmenü; jetzt liegt sie im
    ; Ordner "owncloud.online" des eingebetteten Installers - die alte doppelte
    ; den Eintrag nur
    SetShellVarContext all
    Delete "$SMPROGRAMS\${PRODUCT}.lnk"
    SetShellVarContext current
    Delete "$SMPROGRAMS\${PRODUCT}.lnk"
  ${EndIf}

  ; 7.4 - 7.7 installed to "$PROGRAMFILES64\ownCloud". That directory is also
  ; the home of the official ownCloud client, so it is touched ONLY when
  ; ${OLD_INSTDIR_MARKER} proves an old installation of THIS client - and
  ; only now, after the new install has been VERIFIED in its new location.
  ; prefix check instead of plain equality: a target nested INSIDE the old
  ; directory must not trigger the removal of its own parent either
  StrLen $7 "${OLD_INSTDIR}"
  StrCpy $3 "$INSTDIR" $7
  ${If} ${FileExists} "${OLD_INSTDIR_MARKER}"
  ${AndIf} $9 != ""
  ${AndIf} "$3" != "${OLD_INSTDIR}"
    DetailPrint "Entferne alte owncloud.online-Installation in ${OLD_INSTDIR} / removing old owncloud.online installation ..."
    ; Explorer-Registrierungen der alten Installation (Kennungen des
    ; offiziellen Clients) nur entfernen, soweit sie in den alten Ordner zeigen
    StrCpy $R0 "${OLD_INSTDIR}"
    Call OcoDropLegacyShellExt
    ; The old uninstaller is deliberately NOT executed: its MultiUser
    ; un.onInit re-reads the Install_Dir registry value and overrides _?=
    ; with it. Everything it would clean up is covered here directly.
    ; Files still locked (e.g. shell DLLs loaded by Explorer) are scheduled
    ; for the next restart - which NSIS then flags as pending (finish page /
    ; exit code 3010).
    Delete /REBOOTOK "${OLD_INSTDIR}\uninstall.exe"
    RMDir /r /REBOOTOK "${OLD_INSTDIR}"
    ; the old installation's Start menu entry: its embedded installer created
    ; "$SMPROGRAMS\ownCloud\ownCloud.lnk" (all users) - with the directory
    ; proven to be ours, no other product owns that entry; the folder only
    ; goes when it is empty afterwards (non-recursive)
    SetShellVarContext all
    Delete "$SMPROGRAMS\ownCloud\ownCloud.lnk"
    RMDir "$SMPROGRAMS\ownCloud"
  ${ElseIf} ${FileExists} "${OLD_INSTDIR}\*.*"
  ${AndIfNot} ${FileExists} "${OLD_INSTDIR_MARKER}"
    ; files, but not our marker: another product (the official ownCloud
    ; client) lives there - nothing in it is touched
    DetailPrint "${OLD_INSTDIR} gehoert nicht zu owncloud.online - bleibt unangetastet / does not belong to owncloud.online - left untouched."
  ${EndIf}

  ; ---- Programme und Features über den Helfer -------------------------------
  ; Name, Herausgeber, Link, Symbol und Version schreibt der eingebettete
  ; Installer richtig; nur die Deinstallation läuft über den Helfer
  ; (Explorer-Erweiterungen, Verknüpfungen, Aufräumlauf, Wahlseite).
  ReadRegStr $7 HKLM "${UNINSTKEY}" "DisplayName"
  ${If} $7 != ""
    WriteRegStr HKLM "${UNINSTKEY}" "UninstallString" '"${HELPER_DIR}\${HELPER_EXE}"'
    WriteRegStr HKLM "${UNINSTKEY}" "QuietUninstallString" '"${HELPER_DIR}\${HELPER_EXE}" /S'
  ${Else}
    DetailPrint "WARNUNG: kein Eintrag ${UNINSTKEY} / WARNING: uninstall entry missing."
  ${EndIf}

  ; ---- Desktop shortcut and Explorer integration ----------------------------
  StrCpy $2 "$INSTDIR" ; the dir the embedded installer was steered to
  StrCpy $ClientExe "$9" ; verified above
  ${If} $ClientExe == ""
    ; fallback: the directory the embedded installer recorded for itself
    ReadRegStr $2 HKLM "${REGKEY}" "Install_Dir"
    Call FindClientExe
  ${EndIf}

  ${If} $ClientExe != ""
    DetailPrint "Client-Programmdatei / client binary: $ClientExe"
    ; branded icon if the installer shipped it, else the binary's own icon
    ${If} ${FileExists} "$2\owncloud.ico"
      StrCpy $5 "$2\owncloud.ico"
    ${Else}
      StrCpy $5 "$ClientExe"
    ${EndIf}
    ; Die Startmenü-Verknüpfung legt der eingebettete Installer an; eine
    ; Desktop-Verknüpfung (wie beim alten Client) nur der Wrapper.
    SetShellVarContext all
    ${GetParent} $ClientExe $4
    SetOutPath "$4" ; becomes the shortcut's working directory
    CreateShortCut "$DESKTOP\${PRODUCT}.lnk" "$ClientExe" "" "$5"
    DetailPrint "Desktop-Verknuepfung erstellt / desktop shortcut created."

    ; ---- Register the Explorer shell integration --------------------------
    ; The embedded installer ships OCOverlays.dll and OCContextMenu.dll next
    ; to the client binary but never registers them, so Explorer shows
    ; neither overlay icons nor the context menu. Their DllRegisterServer
    ; writes all required registry entries (own CLSIDs and overlay names, HKLM
    ; 64-bit view). The DLLs are 64-bit, so use the 64-bit regsvr32: from
    ; this 32-bit installer process that is $WINDIR\SysNative; fall back to
    ; $SYSDIR just in case.
    StrCpy $6 "$WINDIR\SysNative\regsvr32.exe"
    ${IfNot} ${FileExists} "$6"
      StrCpy $6 "$SYSDIR\regsvr32.exe"
    ${EndIf}
    ${If} ${FileExists} "$4\OCOverlays.dll"
      DetailPrint "Registriere Explorer-Overlay-Symbole / registering overlay icons ..."
      nsExec::ExecToLog '"$6" /s "$4\OCOverlays.dll"'
      Pop $0
      DetailPrint "OCOverlays.dll: regsvr32 exit code $0"
    ${Else}
      DetailPrint "OCOverlays.dll nicht gefunden / not found in $4 - overlays skipped."
    ${EndIf}
    ${If} ${FileExists} "$4\OCContextMenu.dll"
      DetailPrint "Registriere Explorer-Kontextmenue / registering context menu ..."
      nsExec::ExecToLog '"$6" /s "$4\OCContextMenu.dll"'
      Pop $0
      DetailPrint "OCContextMenu.dll: regsvr32 exit code $0"
    ${Else}
      DetailPrint "OCContextMenu.dll nicht gefunden / not found in $4 - context menu skipped."
    ${EndIf}
  ${Else}
    ; must not happen: the embedded installer reported success but no client
    ; binary can be found - say so loudly instead of finishing silently
    DetailPrint "FEHLER: Client-Programmdatei nicht gefunden / ERROR: client binary not found."
    MessageBox MB_OK|MB_ICONEXCLAMATION "$(TXT_INCOMPLETE)" /SD IDOK
  ${EndIf}
  ; --------------------------------------------------------------------------

  ; Interactive: keep offering the Windows restart on the finish page (the
  ; Explorer integration loads reliably only after one). Silent: leave the
  ; flag to NSIS - it is set only when a locked file had to be scheduled for
  ; the next restart via /REBOOTOK (e.g. old shell DLLs held by Explorer
  ; during an update or a migration) - so the exit code below reports a
  ; pending restart only when there really is one.
  ${IfNot} ${Silent}
    SetRebootFlag true
  ${EndIf}
SectionEnd

Function .onInstSuccess
  ; Silent mode has no finish page. The setup never restarts Windows on its
  ; own: deployment tools (baramundi, Intune, SCCM) manage reboots themselves
  ; and expect the msiexec convention - exit code 3010 = "installed, restart
  ; pending", 0 = nothing pending.
  ;   /S                -> no restart; 3010 if a restart is pending, else 0
  ;   /S /norestart     -> same (kept for existing deployment jobs)
  ;   /S /forcerestart  -> restarts Windows right away (optional)
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
