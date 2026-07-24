/*++

Copyright (c) Xelph. All Rights Reserved.

Module Name:

    allocator.cpp

Abstract:

    Implements placement new/delete for the driver's C++ objects. Kernel
    mode has no CRT heap, so every allocation has to go through the pool
    allocator (ExAllocatePool2/ExFreePoolWithTag) with an explicit tag;
    these overloads let the rest of the driver just write ordinary
    "new (POOL_FLAG_NON_PAGED, TAG) CClass(...)" instead of calling the
    pool APIs by hand everywhere an object is constructed.

Contact:

    Discord: xelphh
    Website: https://Xelph.lol

--*/

#ifdef _NEW_DELETE_OPERATORS_
#ifdef __cplusplus
extern "C" {
#include <wdm.h>
}
#else
#include <wdm.h>
#endif

#include "allocator.h"
#include "shared.h"

#pragma code_seg()

PVOID operator new
(
    size_t      iSize,
    POOL_FLAGS  poolFlags,
    ULONG       tag
)
{
    PVOID result = ExAllocatePool2(poolFlags, iSize, tag);

    return result;
}

PVOID operator new
(
    size_t      iSize,
    POOL_FLAGS  poolFlags
)
{
    PVOID result = ExAllocatePool2(poolFlags, iSize, DRIVER_POOLTAG);

    return result;
}

void __cdecl operator delete
(
    PVOID pVoid,
    ULONG tag
)
{
    if (pVoid)
    {
        ExFreePoolWithTag(pVoid, tag);
    }
}

void __cdecl operator delete
(
    _Pre_maybenull_ __drv_freesMem(Mem) PVOID pVoid,
    _In_ size_t cbSize
)
{
    UNREFERENCED_PARAMETER(cbSize);

    if (pVoid)
    {
        ExFreePoolWithTag(pVoid, DRIVER_POOLTAG);
    }
}

void __cdecl operator delete[]
(
    _Pre_maybenull_ __drv_freesMem(Mem) PVOID pVoid,
    _In_ size_t cbSize
)
{
    UNREFERENCED_PARAMETER(cbSize);

    if (pVoid)
    {
        ExFreePoolWithTag(pVoid, DRIVER_POOLTAG);
    }
}

void __cdecl operator delete[]
(
    _Pre_maybenull_ __drv_freesMem(Mem) PVOID pVoid
)
{
    if (pVoid)
    {
        ExFreePoolWithTag(pVoid, DRIVER_POOLTAG);
    }
}
#endif//_NEW_DELETE_OPERATORS_
