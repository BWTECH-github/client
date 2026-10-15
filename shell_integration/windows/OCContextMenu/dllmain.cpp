/**
* Copyright (c) 2015 Daniel Molkentin <danimo@owncloud.com>. All rights reserved.
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

#include <windows.h>
#include <Guiddef.h>
#include "OCContextMenuRegHandler.h"
#include "OCContextMenuFactory.h"

#include <algorithm>

// {27041A83-BC80-4E2F-AB7C-D6A0F13870CC}
// Eigene Kennung und eigener Handler-Name (Audit H3): {841A0AAD-...} und
// "OCContextMenuHandler" gehören dem offiziellen ownCloud-Client.
static const GUID CLSID_FileContextMenuExt = { 0x27041a83, 0xbc80, 0x4e2f, { 0xab, 0x7c, 0xd6, 0xa0, 0xf1, 0x38, 0x70, 0xcc } };
// Schlüsselname unter AllFileSystemObjects\shellex\ContextMenuHandlers
#define CONTEXT_MENU_HANDLER_NAME L"owncloud.online"

HINSTANCE g_hInst = nullptr;
long        g_cDllRef = 0;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved)
{
    switch (dwReason) {
    case DLL_PROCESS_ATTACH:
        // Hold the instance of this DLL module, we will use it to get the
        // path of the DLL to register the component.
        g_hInst = hModule;
        DisableThreadLibraryCalls(hModule);
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void **ppv)
{
	HRESULT hr = CLASS_E_CLASSNOTAVAILABLE;

	if (IsEqualCLSID(CLSID_FileContextMenuExt, rclsid))	{
		hr = E_OUTOFMEMORY;

		OCContextMenuFactory *pClassFactory = new OCContextMenuFactory();
		if (pClassFactory) {
			hr = pClassFactory->QueryInterface(riid, ppv);
			pClassFactory->Release();
		}
	}

	return hr;
}

STDAPI DllCanUnloadNow(void)
{
	return g_cDllRef > 0 ? S_FALSE : S_OK;
}

STDAPI DllRegisterServer(void)
{
	HRESULT hr;

	wchar_t szModule[MAX_PATH];
	if (GetModuleFileName(g_hInst, szModule, ARRAYSIZE(szModule)) == 0)	{
		hr = HRESULT_FROM_WIN32(GetLastError());
		return hr;
	}

	// Register the component.
	hr = OCContextMenuRegHandler::RegisterInprocServer(szModule, CLSID_FileContextMenuExt,
		L"owncloud.online context menu handler",	L"Apartment");
	if (SUCCEEDED(hr))	{
		// Register the context menu handler. The context menu handler is 
		// associated with the .cpp file class.
		hr = OCContextMenuRegHandler::RegisterShellExtContextMenuHandler(L"AllFileSystemObjects", CLSID_FileContextMenuExt, CONTEXT_MENU_HANDLER_NAME);
	}

	return hr;
}

STDAPI DllUnregisterServer(void)
{
	HRESULT hr = S_OK;

	wchar_t szModule[MAX_PATH];
	if (GetModuleFileName(g_hInst, szModule, ARRAYSIZE(szModule)) == 0)	{
		hr = HRESULT_FROM_WIN32(GetLastError());
		return hr;
	}

	// Unregister the component.
	hr = OCContextMenuRegHandler::UnregisterInprocServer(CLSID_FileContextMenuExt);
	if (SUCCEEDED(hr))	{
		// Unregister the context menu handler.
		hr = OCContextMenuRegHandler::UnregisterShellExtContextMenuHandler(L"AllFileSystemObjects", CONTEXT_MENU_HANDLER_NAME);
	}

	return hr;
}
