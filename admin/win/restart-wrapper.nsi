; owncloud.online client setup wrapper.
;
; Wraps the craft-built NSIS installer and adds what a managed rollout needs:
;  1. stops a running client first, so the installer can actually replace the
;     binaries (a running instance otherwise keeps the old version alive),
;  2. runs the embedded installer silently,
;  3. registers the Explorer integration (overlay icons, context menu) and
;     repairs the shortcuts and the Apps & Features entry,
;  4. reports whether a Windows restart is pending - without ever forcing it.
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
; owncloud.exe, "ownCloud" shortcuts, the autostart entry "ownCloud" - is
; stopped, deleted or changed by this setup.
;
; Build (see .github/workflows/main.yml):
;   makensis /DSOURCE_EXE=<craft-installer.exe> /DOUTFILE=<out.exe> \
;            /DVERSION=<x.y.z> /DUNINSTALL_HELPER_EXE=<helper.exe> \
;            admin/win/restart-wrapper.nsi

Unicode true

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"

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
!define HELPER_EXE "owncloud.online-uninstall.exe"

Name "owncloud.online Client ${VERSION}"
OutFile "${OUTFILE}"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
BrandingText "BW-Tech GmbH - owncloud.online"

; Branded default install directory. A different path can be chosen on the
; directory page, or passed as /D=... (must be the last parameter, unquoted) -
; it is forwarded to the embedded installer. An old installation of THIS
; client in the previous default directory ($PROGRAMFILES64\ownCloud, used by
; 7.4 - 7.7) is removed after the new install has been verified, so upgrades
; move over cleanly (accounts and sync folders live in the user profile and
; are not touched by that).
InstallDir "$PROGRAMFILES64\owncloud.online"
!define OLD_INSTDIR "$PROGRAMFILES64\ownCloud"
; That directory is also the default of the official ownCloud client (MSI
; 6.x: owncloud.exe in the root, craft 7.x: bin\owncloud.exe). Only this
; marker proves that it holds an old installation of THIS client: our builds
; ship bin\owncloud.online.exe since 7.4.0, no other product does. Without
; the marker the directory is left exactly as it is.
!define OLD_INSTDIR_MARKER "${OLD_INSTDIR}\bin\owncloud.online.exe"

!define CLIENT_PROCESS "owncloud.online.exe"

!define MUI_ICON "..\..\src\resources\theme\universal\owncloud-online.ico"

; The finish page shows the reboot-now/-later choice whenever the reboot flag
; is set: in interactive mode the install section sets it (the Explorer
; integration loads reliably only after a restart); in silent mode only NSIS
; itself sets it, when a locked file had to be scheduled via /REBOOTOK.
!define MUI_FINISHPAGE_TEXT_REBOOT "Um die Installation abzuschliessen, muss Windows neu gestartet werden, damit die Explorer-Integration korrekt funktioniert.$\r$\n$\r$\nTo complete the installation, Windows must be restarted so that the Explorer integration works correctly."
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_LANGUAGE "German"
!insertmacro MUI_LANGUAGE "English"

Var ClientExe

; Snapshot of the upstream-named Start menu shortcuts taken BEFORE the
; embedded installer runs ("1" = existed already). That installer creates
; "$SMPROGRAMS\ownCloud\ownCloud.lnk" pointing at the non-existent
; bin\owncloud.exe. Only shortcuts that APPEAR during this run are removed
; afterwards: an "ownCloud" shortcut that already existed belongs to the
; official ownCloud client and is left alone.
Var PreLnkAllRoot   ; all users:    $SMPROGRAMS\ownCloud.lnk
Var PreLnkAllDir    ; all users:    $SMPROGRAMS\ownCloud\ownCloud.lnk
Var PreDirAll       ; all users:    $SMPROGRAMS\ownCloud (folder)
Var PreLnkCurRoot   ; current user: $SMPROGRAMS\ownCloud.lnk
Var PreLnkCurDir    ; current user: $SMPROGRAMS\ownCloud\ownCloud.lnk
Var PreDirCur       ; current user: $SMPROGRAMS\ownCloud (folder)

; ${ExistsFlag} path var -> var = "1" if path exists right now, else "0"
!macro ExistsFlag _path _var
  ${If} ${FileExists} "${_path}"
    StrCpy ${_var} "1"
  ${Else}
    StrCpy ${_var} "0"
  ${EndIf}
!macroend
!define ExistsFlag "!insertmacro ExistsFlag"

; ${DeleteIfNew} path flag -> delete the shortcut only if it did NOT exist
; before the embedded installer ran (flag "0"); a pre-existing one is never
; touched
!macro DeleteIfNew _path _flag
  ${If} ${_flag} == "0"
  ${AndIf} ${FileExists} "${_path}"
    DetailPrint "Entferne vom Basis-Installer angelegte Verknuepfung / removing shortcut created by the embedded installer: ${_path}"
    Delete "${_path}"
  ${EndIf}
!macroend
!define DeleteIfNew "!insertmacro DeleteIfNew"

; ${RMDirIfNew} path flag -> remove the folder only if the embedded installer
; created it (flag "0"); non-recursive, so only an empty folder goes
!macro RMDirIfNew _path _flag
  ${If} ${_flag} == "0"
    RMDir "${_path}"
  ${EndIf}
!macroend
!define RMDirIfNew "!insertmacro RMDirIfNew"

; in: $2 = candidate install dir. out: $ClientExe = full path of the client
; binary, or "" if none found. Only this client's own binary name counts:
; bin\owncloud.online.exe (since 7.4.0) or owncloud.online.exe (flat
; layout). The upstream name owncloud.exe is the official ownCloud client
; and is never adopted, registered or linked to.
Function FindClientExe
  ${If} ${FileExists} "$2\bin\owncloud.online.exe"
    StrCpy $ClientExe "$2\bin\owncloud.online.exe"
  ${ElseIf} ${FileExists} "$2\owncloud.online.exe"
    StrCpy $ClientExe "$2\owncloud.online.exe"
  ${Else}
    StrCpy $ClientExe ""
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
  ; value from HKLM "Software\ownCloud GmbH\ownCloud" (32-bit view) when
  ; present. Writing that value beforehand is the only reliable way to choose
  ; its target directory - it is the same mechanism its own upgrades use.
  ; (This wrapper is a 32-bit process, so a plain HKLM write lands in the same
  ; WOW6432Node view the embedded installer reads.)
  ; $8 remembers the previous value so a failed install can restore it.
  ReadRegStr $8 HKLM "Software\ownCloud GmbH\ownCloud" "Install_Dir"
  WriteRegStr HKLM "Software\ownCloud GmbH\ownCloud" "Install_Dir" "$INSTDIR"

  ; Unpack and run the real installer silently.
  InitPluginsDir
  SetOutPath "$PLUGINSDIR"
  File "/oname=client-setup.exe" "${SOURCE_EXE}"
  ; remember which upstream-named shortcuts exist BEFORE the embedded
  ; installer runs - only the ones it creates now are removed later (see the
  ; Var block above); the embedded installer runs per-machine, the current
  ; user context is covered for completeness only
  SetShellVarContext all
  ${ExistsFlag} "$SMPROGRAMS\ownCloud.lnk" $PreLnkAllRoot
  ${ExistsFlag} "$SMPROGRAMS\ownCloud\ownCloud.lnk" $PreLnkAllDir
  ${ExistsFlag} "$SMPROGRAMS\ownCloud" $PreDirAll
  SetShellVarContext current
  ${ExistsFlag} "$SMPROGRAMS\ownCloud.lnk" $PreLnkCurRoot
  ${ExistsFlag} "$SMPROGRAMS\ownCloud\ownCloud.lnk" $PreLnkCurDir
  ${ExistsFlag} "$SMPROGRAMS\ownCloud" $PreDirCur
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
      WriteRegStr HKLM "Software\ownCloud GmbH\ownCloud" "Install_Dir" "$8"
    ${Else}
      DeleteRegValue HKLM "Software\ownCloud GmbH\ownCloud" "Install_Dir"
    ${EndIf}
    Abort "Installation fehlgeschlagen / installation failed (exit code $0)."
  installOk:

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
  ; then run elevated. $R7 carries the directory for the UninstallString
  ; rewrites below.
  StrCpy $R7 "$PROGRAMFILES64\owncloud.online-uninstall"
  DetailPrint "Installiere Deinstallations-Helfer / installing uninstall helper ..."
  CreateDirectory "$R7"
  SetOutPath "$R7"
  File "/oname=${HELPER_EXE}" "${UNINSTALL_HELPER_EXE}"
  ; remove the misplaced 7.10.2-generation helper from the install dir
  Delete "$INSTDIR\${HELPER_EXE}"

  ; ---- Migrate away from the old default directory --------------------------
  ; 7.4 - 7.7 installed to "$PROGRAMFILES64\ownCloud". That directory is also
  ; the home of the official ownCloud client, so it is touched ONLY when
  ; ${OLD_INSTDIR_MARKER} proves an old installation of THIS client - and
  ; only now, after the new install has been VERIFIED in its new location.
  ; The old uninstaller deletes the shared Uninstall\ownCloud and
  ; "ownCloud GmbH" registry keys, so the entries for the NEW install are
  ; rewritten right afterwards.
  ; prefix check instead of plain equality: a target nested INSIDE the old
  ; directory must not trigger the removal of its own parent either
  StrLen $7 "${OLD_INSTDIR}"
  StrCpy $3 "$INSTDIR" $7
  ${If} ${FileExists} "${OLD_INSTDIR_MARKER}"
  ${AndIf} $9 != ""
  ${AndIf} "$3" != "${OLD_INSTDIR}"
    DetailPrint "Entferne alte owncloud.online-Installation in ${OLD_INSTDIR} / removing old owncloud.online installation ..."
    ; unregister the old shell extensions first - Explorer holds those DLLs
    StrCpy $6 "$WINDIR\SysNative\regsvr32.exe"
    ${IfNot} ${FileExists} "$6"
      StrCpy $6 "$SYSDIR\regsvr32.exe"
    ${EndIf}
    ${If} ${FileExists} "${OLD_INSTDIR}\bin\OCOverlays.dll"
      nsExec::ExecToLog '"$6" /u /s "${OLD_INSTDIR}\bin\OCOverlays.dll"'
      Pop $0
    ${EndIf}
    ${If} ${FileExists} "${OLD_INSTDIR}\bin\OCContextMenu.dll"
      nsExec::ExecToLog '"$6" /u /s "${OLD_INSTDIR}\bin\OCContextMenu.dll"'
      Pop $0
    ${EndIf}
    ; The old uninstaller is deliberately NOT executed: its MultiUser
    ; un.onInit re-reads the Install_Dir registry value - which now points at
    ; the NEW installation - and overrides _?= with it, so it would delete
    ; the client we just installed. Everything it would clean up is covered
    ; here directly: the files below, the shared registry entries right after.
    ; Files still locked (e.g. shell DLLs loaded by Explorer) are scheduled
    ; for the next restart - which NSIS then flags as pending (finish page /
    ; exit code 3010).
    Delete /REBOOTOK "${OLD_INSTDIR}\uninstall.exe"
    RMDir /r /REBOOTOK "${OLD_INSTDIR}"
    ; the old installation's Start menu entry: its embedded installer created
    ; "$SMPROGRAMS\ownCloud\ownCloud.lnk" (all users) and the embedded
    ; installer of THIS run has just overwritten it - with the directory
    ; proven to be ours, no other product owns that entry; the folder only
    ; goes when it is empty afterwards (non-recursive)
    SetShellVarContext all
    Delete "$SMPROGRAMS\ownCloud\ownCloud.lnk"
    RMDir "$SMPROGRAMS\ownCloud"
    ; make sure the registry entries describe the NEW installation (the
    ; embedded installer generation writes in the default 32-bit view)
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "DisplayName" "owncloud.online Client ${VERSION}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "UninstallString" '"$R7\${HELPER_EXE}"'
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "QuietUninstallString" '"$R7\${HELPER_EXE}" /S'
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "DisplayVersion" "${VERSION}"
    WriteRegStr HKLM "Software\ownCloud GmbH\ownCloud" "Install_Dir" "$INSTDIR"
  ${ElseIf} ${FileExists} "${OLD_INSTDIR}\*.*"
  ${AndIfNot} ${FileExists} "${OLD_INSTDIR_MARKER}"
    ; files, but not our marker: another product (the official ownCloud
    ; client) lives there - nothing in it is touched
    DetailPrint "${OLD_INSTDIR} gehoert nicht zu owncloud.online - bleibt unangetastet / does not belong to owncloud.online - left untouched."
  ${EndIf}

  ; ---- Rebrand the Apps & Features entry ------------------------------------
  ; The embedded installer writes its uninstall entry with upstream branding
  ; ("ownCloud", Publisher "ownCloud GmbH"). Rewrite the user-visible values in
  ; whichever registry view the entry landed in.
  DetailPrint "Aktualisiere Programme-und-Features-Eintrag / rebranding uninstall entry ..."
  ReadRegStr $7 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "DisplayName"
  ${If} $7 != ""
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "DisplayName" "owncloud.online Client ${VERSION}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "Publisher" "BW.Tech GmbH"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "URLInfoAbout" "https://github.com/GrossLukas/client"
    ; route Apps & Features through the uninstall helper (shortcuts, shell
    ; extensions and the restart requirement are handled there)
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "UninstallString" '"$R7\${HELPER_EXE}"'
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "QuietUninstallString" '"$R7\${HELPER_EXE}" /S'
  ${EndIf}
  SetRegView 64
  ReadRegStr $7 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "DisplayName"
  ${If} $7 != ""
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "DisplayName" "owncloud.online Client ${VERSION}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "Publisher" "BW.Tech GmbH"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "URLInfoAbout" "https://github.com/GrossLukas/client"
    ; route Apps & Features through the uninstall helper (shortcuts, shell
    ; extensions and the restart requirement are handled there)
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "UninstallString" '"$R7\${HELPER_EXE}"'
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "QuietUninstallString" '"$R7\${HELPER_EXE}" /S'
  ${EndIf}
  SetRegView 32

  ; ---- Repair the shortcuts -----------------------------------------------
  ; The embedded installer hardcodes its Start menu shortcut to
  ; "$INSTDIR\bin\owncloud.exe" (the upstream binary name) and creates no
  ; desktop shortcut. The real binary is bin\owncloud.online.exe (or
  ; owncloud.online.exe in a flat layout): find it and create correctly
  ; branded, working shortcuts.

  StrCpy $2 "$INSTDIR" ; the dir the embedded installer was steered to
  StrCpy $ClientExe "$9" ; verified above
  ${If} $ClientExe == ""
    ; fallback: resolve the install dir from the uninstall entry (only a
    ; directory holding OUR binary name is accepted, see FindClientExe - the
    ; entry is shared with the official craft-built ownCloud client)
    ReadRegStr $1 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "UninstallString"
    ${If} $1 == ""
      SetRegView 64
      ReadRegStr $1 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "UninstallString"
      SetRegView 32
    ${EndIf}
    ${If} $1 != ""
      StrCpy $4 $1 1
      ${If} $4 == '"'
        StrCpy $1 $1 -1 1 ; strip the surrounding quotes
      ${EndIf}
      ${GetParent} $1 $2
      Call FindClientExe
    ${EndIf}
  ${EndIf}

  ${If} $ClientExe != ""
    DetailPrint "Client-Programmdatei / client binary: $ClientExe"
    ; remove the broken upstream-named Start menu shortcut the embedded
    ; installer created during THIS run - and only that one: whatever
    ; existed before (snapshot taken before ExecWait) belongs to the official
    ; ownCloud client and stays, as does any "ownCloud" desktop shortcut
    SetShellVarContext all
    ${DeleteIfNew} "$SMPROGRAMS\ownCloud.lnk" $PreLnkAllRoot
    ${DeleteIfNew} "$SMPROGRAMS\ownCloud\ownCloud.lnk" $PreLnkAllDir
    ${RMDirIfNew} "$SMPROGRAMS\ownCloud" $PreDirAll
    SetShellVarContext current
    ${DeleteIfNew} "$SMPROGRAMS\ownCloud.lnk" $PreLnkCurRoot
    ${DeleteIfNew} "$SMPROGRAMS\ownCloud\ownCloud.lnk" $PreLnkCurDir
    ${RMDirIfNew} "$SMPROGRAMS\ownCloud" $PreDirCur
    ; branded icon if the installer shipped it, else the binary's own icon
    ${If} ${FileExists} "$2\owncloud.ico"
      StrCpy $5 "$2\owncloud.ico"
    ${Else}
      StrCpy $5 "$ClientExe"
    ${EndIf}
    ; create the branded, working shortcuts for all users
    SetShellVarContext all
    ${GetParent} $ClientExe $4
    SetOutPath "$4" ; becomes the shortcuts' working directory
    CreateShortCut "$SMPROGRAMS\owncloud.online.lnk" "$ClientExe" "" "$5"
    CreateShortCut "$DESKTOP\owncloud.online.lnk" "$ClientExe" "" "$5"
    DetailPrint "Startmenue- und Desktop-Verknuepfung erstellt / start menu and desktop shortcuts created."

    ; branded icon for the Apps & Features entry too
    ReadRegStr $7 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "DisplayName"
    ${If} $7 != ""
      WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "DisplayIcon" "$5"
    ${EndIf}
    SetRegView 64
    ReadRegStr $7 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "DisplayName"
    ${If} $7 != ""
      WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud" "DisplayIcon" "$5"
    ${EndIf}
    SetRegView 32

    ; ---- Register the Explorer shell integration --------------------------
    ; The embedded installer ships OCOverlays.dll and OCContextMenu.dll next
    ; to the client binary but never registers them, so Explorer shows
    ; neither overlay icons nor the context menu. Their DllRegisterServer
    ; writes all required registry entries (CLSIDs and the
    ; ShellIconOverlayIdentifiers keys, HKLM 64-bit view). The DLLs are
    ; 64-bit, so use the 64-bit regsvr32: from this 32-bit installer process
    ; that is $WINDIR\SysNative; fall back to $SYSDIR just in case.
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
    MessageBox MB_OK|MB_ICONEXCLAMATION "Die Installation scheint unvollstaendig zu sein: die Client-Programmdatei wurde nicht gefunden. Bitte den Setup erneut ausfuehren und andernfalls den Support kontaktieren.$\r$\n$\r$\nThe installation appears to be incomplete: the client binary was not found. Please re-run the setup." /SD IDOK
  ${EndIf}
  ; --------------------------------------------------------------------------

  ; Interactive: keep offering the Windows restart on the finish page (the
  ; Explorer integration loads reliably only after one). Silent: leave the
  ; flag to NSIS - it is set only when a locked file had to be scheduled for
  ; the next restart via /REBOOTOK (e.g. old shell DLLs held by Explorer
  ; during a migration) - so the exit code below reports a pending restart
  ; only when there really is one.
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
