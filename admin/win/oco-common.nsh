; owncloud.online: gemeinsame Namen und Altlast-Behandlung für restart-wrapper.nsi
; und uninstall-helper.nsi. Texte für die Oberfläche stehen in oco-texts.nsh.
;
; Beide Programme sind 32-Bit-NSIS-Prozesse: HKLM ohne SetRegView landet in
; WOW6432Node - dort schreibt auch der eingebettete craft-Installer (32 Bit).
; Funktionen hier verändern nur $R0-$R6 und stellen SetRegView 32 wieder her.

!include "LogicLib.nsh"
!include "FileFunc.nsh"

!define PRODUCT "owncloud.online"
!define CLIENT_PROCESS "owncloud.online.exe"
!define HELPER_EXE "owncloud.online-uninstall.exe"
; Ordner des Deinstallations-Helfers NEBEN der Installation (siehe Wrapper)
!define HELPER_DIR "$PROGRAMFILES64\owncloud.online-uninstall"

; Identität des eingebetteten craft-Installers (Blueprint-Fork
; BWTECH-github/craft-blueprints-owncloud, Zweig bwtech): Software\<company>\
; <productname> mit Install_Dir und ...\Uninstall\<productname>.
!define VENDORKEY "Software\BW-Tech GmbH"
!define REGKEY "${VENDORKEY}\${PRODUCT}"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT}"

; Richtlinien (GPO), die der Client liest: HKLM\Software\Policies\<Hersteller>\
; <Produkt> (AppConfig::configPath, Hersteller und Name aus OWNCLOUD.cmake)
!define POLICY_VENDORKEY "Software\Policies\BW-Tech GmbH"
!define POLICYKEY "${POLICY_VENDORKEY}\${PRODUCT}"

; Identität bis 7.11 (upstream-Blueprint). Diese Namen teilt der offizielle
; ownCloud-Client - sein MSI (6.x) startet z. B. den UninstallString von
; Uninstall\ownCloud bei jeder Installation, Reparatur und Deinstallation als
; vermeintliche Altinstallation (Install-Probe K6). Entfernt wird daher nur,
; was nachweislich zu diesem Client gehört.
!define LEGACY_VENDORKEY "Software\ownCloud GmbH"
!define LEGACY_REGKEY "${LEGACY_VENDORKEY}\ownCloud"
!define LEGACY_UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\ownCloud"

!define OVERLAY_KEY "SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\ShellIconOverlayIdentifiers"
!define MENU_HANDLER_KEY "Software\Classes\AllFileSystemObjects\shellex\ContextMenuHandlers"

; in: $R0 = Ordner. out: $R1 = "1", wenn dort dieser Client liegt
; (bin\owncloud.online.exe seit 7.4.0 oder flach owncloud.online.exe). Den
; Namen owncloud.exe trägt nur der offizielle Client.
Function OcoHasClient
  StrCpy $R1 "0"
  ${If} $R0 != ""
    ${If} ${FileExists} "$R0\bin\${CLIENT_PROCESS}"
    ${OrIf} ${FileExists} "$R0\${CLIENT_PROCESS}"
      StrCpy $R1 "1"
    ${EndIf}
  ${EndIf}
FunctionEnd

; Registrierung einer Explorer-Erweiterung aus 7.x (CLSIDs und Namen des
; offiziellen Clients, Audit H3) entfernen - nur wenn ihr InprocServer32 in
; $R0 oder $R0\bin liegt. Zeigt sie auf den offiziellen Client, bleibt sie.
; Erwartet SetRegView 64.
!macro _OcoDropLegacyOverlay _clsid _name
  ReadRegStr $R2 HKLM "Software\Classes\CLSID\${_clsid}\InprocServer32" ""
  ${If} $R2 != ""
    ${GetParent} "$R2" $R3
    ${If} $R3 == "$R0\bin"
    ${OrIf} $R3 == "$R0"
      DetailPrint "Entferne alte Overlay-Registrierung / removing legacy overlay registration ${_clsid}"
      ReadRegStr $R4 HKLM "${OVERLAY_KEY}\${_name}" ""
      ${If} $R4 == "${_clsid}"
        DeleteRegKey HKLM "${OVERLAY_KEY}\${_name}"
      ${EndIf}
      DeleteRegKey HKLM "Software\Classes\CLSID\${_clsid}"
    ${EndIf}
  ${EndIf}
!macroend

; in: $R0 = Installationsordner (ohne abschließenden Backslash)
Function OcoDropLegacyShellExt
  SetRegView 64
  !insertmacro _OcoDropLegacyOverlay "{0960F090-F328-48A3-B746-276B1E3C3722}" "  OCError"
  !insertmacro _OcoDropLegacyOverlay "{0960F091-F328-48A3-B746-276B1E3C3722}" "  OCErrorShared"
  !insertmacro _OcoDropLegacyOverlay "{0960F092-F328-48A3-B746-276B1E3C3722}" "  OCOK"
  !insertmacro _OcoDropLegacyOverlay "{0960F093-F328-48A3-B746-276B1E3C3722}" "  OCOKShared"
  !insertmacro _OcoDropLegacyOverlay "{0960F094-F328-48A3-B746-276B1E3C3722}" "  OCSync"
  !insertmacro _OcoDropLegacyOverlay "{0960F095-F328-48A3-B746-276B1E3C3722}" "  OCSyncShared"
  !insertmacro _OcoDropLegacyOverlay "{0960F096-F328-48A3-B746-276B1E3C3722}" "  OCWarning"
  !insertmacro _OcoDropLegacyOverlay "{0960F097-F328-48A3-B746-276B1E3C3722}" "  OCWarningShared"
  ; Kontextmenü: Handler-Schlüssel nur, wenn er auf die eigene Kennung zeigt
  !define _OCO_MENU_CLSID "{841A0AAD-AA11-4B50-84D9-7F8E727D77D7}"
  ReadRegStr $R2 HKLM "Software\Classes\CLSID\${_OCO_MENU_CLSID}\InprocServer32" ""
  ${If} $R2 != ""
    ${GetParent} "$R2" $R3
    ${If} $R3 == "$R0\bin"
    ${OrIf} $R3 == "$R0"
      DetailPrint "Entferne alte Kontextmenue-Registrierung / removing legacy context menu registration"
      ReadRegStr $R4 HKLM "${MENU_HANDLER_KEY}\OCContextMenuHandler" ""
      ${If} $R4 == "${_OCO_MENU_CLSID}"
        DeleteRegKey HKLM "${MENU_HANDLER_KEY}\OCContextMenuHandler"
      ${EndIf}
      DeleteRegKey HKLM "Software\Classes\CLSID\${_OCO_MENU_CLSID}"
    ${EndIf}
  ${EndIf}
  !undef _OCO_MENU_CLSID
  SetRegView 32
FunctionEnd

; Altschlüssel aus 7.x in einer Wurzel (HKLM/HKCU) und der aktuellen Ansicht
; entfernen, wenn sie zu diesem Client gehören:
; - Uninstall\ownCloud: DisplayName beginnt mit owncloud.online (seit 7.4
;   umbenannt), der UninstallString liegt im Helfer-Ordner oder in einem
;   Ordner mit diesem Client;
; - Software\ownCloud GmbH\ownCloud: Install_Dir enthält diesen Client.
; Aufrufen, solange die alte Installation noch auf der Platte liegt.
!macro _OcoDropLegacyKeysIn _root
  StrCpy $R5 "0"
  ReadRegStr $R2 ${_root} "${LEGACY_UNINSTKEY}" "DisplayName"
  StrCpy $R2 $R2 15
  ${If} $R2 == "${PRODUCT}"
    StrCpy $R5 "1"
  ${EndIf}
  ReadRegStr $R2 ${_root} "${LEGACY_UNINSTKEY}" "UninstallString"
  ${If} $R5 == "0"
  ${AndIf} $R2 != ""
    StrCpy $R3 $R2 1
    ${If} $R3 == '"'
      StrCpy $R2 $R2 "" 1
    ${EndIf}
    ${GetParent} "$R2" $R0
    ${If} $R0 == "${HELPER_DIR}"
      StrCpy $R5 "1"
    ${Else}
      Call OcoHasClient
      StrCpy $R5 $R1
    ${EndIf}
  ${EndIf}
  ${If} $R5 == "1"
    DetailPrint "Entferne alten Programme-und-Features-Eintrag / removing legacy uninstall entry ${_root}\${LEGACY_UNINSTKEY}"
    DeleteRegKey ${_root} "${LEGACY_UNINSTKEY}"
  ${EndIf}
  ReadRegStr $R0 ${_root} "${LEGACY_REGKEY}" "Install_Dir"
  Call OcoHasClient
  ${If} $R1 == "1"
    DetailPrint "Entferne alten Registrierungsschluessel / removing legacy key ${_root}\${LEGACY_REGKEY}"
    DeleteRegKey ${_root} "${LEGACY_REGKEY}"
    DeleteRegKey /ifempty ${_root} "${LEGACY_VENDORKEY}"
  ${EndIf}
!macroend

Function OcoDropLegacyKeys
  !insertmacro _OcoDropLegacyKeysIn HKLM
  !insertmacro _OcoDropLegacyKeysIn HKCU
  SetRegView 64
  !insertmacro _OcoDropLegacyKeysIn HKLM
  !insertmacro _OcoDropLegacyKeysIn HKCU
  SetRegView 32
FunctionEnd
