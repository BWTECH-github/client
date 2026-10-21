/**
* Copyright (c) 2000-2013 Liferay, Inc. All rights reserved.
* Modified by BW-Tech GmbH for owncloud.online branding.
*
* This library is free software; you can redistribute it and/or modify it under
* the terms of the GNU Lesser General Public License as published by the Free
* Software Foundation; either version 2.1 of the License, or (at your option)
* any later version.
*
* This library is distributed in the hope that it will be useful, but WITHOUT
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
* FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
* details.
*/


#define OVERLAY_GUID_ERROR          L"{CCD18E20-5D74-4A54-AFCF-65346B89869F}"
#define OVERLAY_GUID_ERROR_SHARED   L"{CCD18E21-5D74-4A54-AFCF-65346B89869F}"
#define OVERLAY_GUID_OK             L"{CCD18E22-5D74-4A54-AFCF-65346B89869F}"
#define OVERLAY_GUID_OK_SHARED      L"{CCD18E23-5D74-4A54-AFCF-65346B89869F}"
#define OVERLAY_GUID_SYNC           L"{CCD18E24-5D74-4A54-AFCF-65346B89869F}"
#define OVERLAY_GUID_SYNC_SHARED    L"{CCD18E25-5D74-4A54-AFCF-65346B89869F}"
#define OVERLAY_GUID_WARNING        L"{CCD18E26-5D74-4A54-AFCF-65346B89869F}"
#define OVERLAY_GUID_WARNING_SHARED L"{CCD18E27-5D74-4A54-AFCF-65346B89869F}"

#define OVERLAY_GENERIC_NAME        L"owncloud.online overlay handler"

// Eigene CLSIDs und Overlay-Namen (Audit H3): {0960F090..7-...} und "  OCError" usw.
// gehören dem offiziellen ownCloud-Client - geteilte Kennungen überschrieben bei der
// Installation dessen Registrierung und entfernten sie bei der Deinstallation.
// two spaces to put us ahead of the competition :/
#define OVERLAY_NAME_ERROR          L"  OCOError"
#define OVERLAY_NAME_ERROR_SHARED   L"  OCOErrorShared"
#define OVERLAY_NAME_OK             L"  OCOOK"
#define OVERLAY_NAME_OK_SHARED      L"  OCOOKShared"
#define OVERLAY_NAME_SYNC           L"  OCOSync"
#define OVERLAY_NAME_SYNC_SHARED    L"  OCOSyncShared"
#define OVERLAY_NAME_WARNING        L"  OCOWarning"
#define OVERLAY_NAME_WARNING_SHARED L"  OCOWarningShared"

#define REGISTRY_OVERLAY_KEY        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellIconOverlayIdentifiers"
#define REGISTRY_CLSID              L"CLSID"
#define REGISTRY_IN_PROCESS         L"InprocServer32"
#define REGISTRY_THREADING          L"ThreadingModel"
#define REGISTRY_APARTMENT          L"Apartment"
#define REGISTRY_VERSION            L"Version"
#define REGISTRY_VERSION_NUMBER     L"1.0"

//Registry values for running
#define REGISTRY_ENABLE_OVERLAY     L"EnableOverlay"

#define GET_FILE_OVERLAY_ID     L"getFileIconId"

#define PORT                34001
