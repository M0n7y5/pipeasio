/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2006 Robert Reif
 * Portions copyright (C) 2026 PipeASIO contributors
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "winreg.h"
#include "objbase.h"
#include "unknwn.h"

#if defined(DEBUG) && !defined(PIPEASIO_WOW64_PE)
#include "wine/debug.h"
#endif

/* {2d3ca9e2-1193-4c5d-b5fd-38798f3dc074} */
static GUID const CLSID_PipeASIO
        = { 0x2d3ca9e2, 0x1193, 0x4c5d, { 0xb5, 0xfd, 0x38, 0x79, 0x8f, 0x3d, 0xc0, 0x74 } };

typedef struct
{
    const IClassFactoryVtbl *lpVtbl;
    LONG                     ref;
} IClassFactoryImpl;
static LONG driver_objects;
static LONG server_locks;

void
pipeasio_object_created(void)
{
    InterlockedIncrement(&driver_objects);
}

void
pipeasio_object_destroyed(void)
{
    InterlockedDecrement(&driver_objects);
}

extern HRESULT WINAPI PipeASIOCreateInstance(REFIID riid, LPVOID *ppobj);

/*******************************************************************************
 * ClassFactory
 */
static ULONG WINAPI CF_AddRef(LPCLASSFACTORY iface);

static HRESULT WINAPI
CF_QueryInterface(LPCLASSFACTORY iface, REFIID riid, LPVOID *ppobj)
{
    if (!ppobj)
        return E_POINTER;
    *ppobj = NULL;
    if (!IsEqualIID(riid, &IID_IUnknown) && !IsEqualIID(riid, &IID_IClassFactory))
        return E_NOINTERFACE;
    CF_AddRef(iface);
    *ppobj = iface;
    return S_OK;
}

static ULONG WINAPI
CF_AddRef(LPCLASSFACTORY iface)
{
    IClassFactoryImpl *This = (IClassFactoryImpl *)iface;
    ULONG              ref  = InterlockedIncrement(&(This->ref));
    return ref;
}

static ULONG WINAPI
CF_Release(LPCLASSFACTORY iface)
{
    IClassFactoryImpl *This = (IClassFactoryImpl *)iface;
    LONG               ref  = InterlockedCompareExchange(&This->ref, 0, 0);
    while (ref > 1)
    {
        LONG prior = InterlockedCompareExchange(&This->ref, ref - 1, ref);
        if (prior == ref)
            return (ULONG)(ref - 1);
        ref = prior;
    }
    return 1;
}

static HRESULT WINAPI
CF_CreateInstance(LPCLASSFACTORY iface, LPUNKNOWN pOuter, REFIID riid, LPVOID *ppobj)
{
    (void)iface;
    (void)riid;
    if (pOuter)
        return CLASS_E_NOAGGREGATION;

    if (!ppobj)
        return E_POINTER;

    *ppobj = NULL;
    return PipeASIOCreateInstance(riid, ppobj);
}

static HRESULT WINAPI
CF_LockServer(LPCLASSFACTORY iface, BOOL dolock)
{
    (void)iface;
    if (dolock)
        InterlockedIncrement(&server_locks);
    else
    {
        LONG locks = InterlockedCompareExchange(&server_locks, 0, 0);
        while (locks > 0)
        {
            LONG prior = InterlockedCompareExchange(&server_locks, locks - 1, locks);
            if (prior == locks)
                break;
            locks = prior;
        }
    }
    return S_OK;
}

static const IClassFactoryVtbl CF_Vtbl
        = { CF_QueryInterface, CF_AddRef, CF_Release, CF_CreateInstance, CF_LockServer };

static IClassFactoryImpl PIPEASIO_CF = { &CF_Vtbl, 1 };

/*******************************************************************************
 * DllGetClassObject
 * Retrieves class object from a DLL object
 *
 * RETURNS
 *    Success: S_OK
 *    Failure: CLASS_E_CLASSNOTAVAILABLE, E_OUTOFMEMORY, E_INVALIDARG,
 *             E_UNEXPECTED
 */
HRESULT WINAPI
DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv)
{
    if (!ppv)
        return E_POINTER;

    *ppv = NULL;

    if (!IsEqualIID(riid, &IID_IClassFactory) && !IsEqualIID(riid, &IID_IUnknown))
    {
        return E_NOINTERFACE;
    }

    if (IsEqualGUID(rclsid, &CLSID_PipeASIO))
    {
        CF_AddRef((IClassFactory *)&PIPEASIO_CF);
        *ppv = &PIPEASIO_CF;
        return S_OK;
    }

    return CLASS_E_CLASSNOTAVAILABLE;
}

/*******************************************************************************
 * DllCanUnloadNow
 * Determines whether the DLL is in use.
 *
 * RETURNS
 *    Success: S_OK
 *    Failure: S_FALSE
 */
HRESULT WINAPI
DllCanUnloadNow(void)
{
    LONG factory_refs = InterlockedCompareExchange(&PIPEASIO_CF.ref, 0, 0);
    LONG objects      = InterlockedCompareExchange(&driver_objects, 0, 0);
    LONG locks        = InterlockedCompareExchange(&server_locks, 0, 0);
    return objects == 0 && locks == 0 && factory_refs == 1 ? S_OK : S_FALSE;
}

/***********************************************************************
 *           DllMain (ASIO.init)
 */
BOOL WINAPI
DllMain(HINSTANCE hInstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)hInstDLL;
    (void)lpvReserved;
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        break;
    case DLL_PROCESS_DETACH:
        break;
    case DLL_THREAD_ATTACH:
        break;
    case DLL_THREAD_DETACH:
        break;
    default:
        break;
    }
    return TRUE;
}
