/*++

Copyright (c) Xelph. All Rights Reserved.

Module Name:

    inputendpoint.h

Abstract:

    Declaration of CMicArrayMiniportTopology, the microphone topology
    miniport (see inputendpoint.cpp).

Contact:

    Discord: xelphh
    Website: https://Xelph.lol

--*/

#ifndef _INPUTENDPOINT_H_
#define _INPUTENDPOINT_H_

#include "topologybase.h"

#pragma code_seg()
class CMicArrayMiniportTopology :
    public CTopologyBase,
    public IMiniportTopology,
    public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    CMicArrayMiniportTopology
    (
        _In_opt_    PUNKNOWN                UnknownOuter,
        _In_        PCFILTER_DESCRIPTOR* FilterDesc,
        _In_        USHORT                  DeviceMaxChannels,
        _In_        eDeviceType             DeviceType
    )
        : CUnknown(UnknownOuter),
        CTopologyBase(FilterDesc, DeviceMaxChannels),
        m_DeviceType(DeviceType)
    {
        ASSERT(m_DeviceType == eMicArrayDevice1);
    }

    ~CMicArrayMiniportTopology();

    IMP_IMiniportTopology;

    NTSTATUS            PropertyHandlerMicArrayGeometry
    (
        _In_ PPCPROPERTY_REQUEST  PropertyRequest
    );

    NTSTATUS            PropertyHandlerMicProperties
    (
        _In_ PPCPROPERTY_REQUEST  PropertyRequest
    );

    NTSTATUS            PropertyHandlerJackDescription
    (
        _In_ PPCPROPERTY_REQUEST  PropertyRequest
    );

    NTSTATUS            PropertyHandlerJackDescription2
    (
        _In_ PPCPROPERTY_REQUEST  PropertyRequest
    );

protected:
    eDeviceType     m_DeviceType;

};

typedef CMicArrayMiniportTopology* PCMicArrayMiniportTopology;


NTSTATUS
CreateMicArrayMiniportTopology(
    _Out_           PUNKNOWN* Unknown,
    _In_            REFCLSID,
    _In_opt_        PUNKNOWN                                UnknownOuter,
    _In_            POOL_FLAGS                              PoolFlags,
    _In_            PUNKNOWN                                UnknownAdapter,
    _In_opt_        PVOID                                   DeviceContext,
    _In_            PENDPOINT_MINIPAIR                      MiniportPair
);

NTSTATUS PropertyHandler_MicArrayTopoFilter(_In_ PPCPROPERTY_REQUEST PropertyRequest);
NTSTATUS PropertyHandler_MicArrayTopology(_In_ PPCPROPERTY_REQUEST PropertyRequest);

#endif // _INPUTENDPOINT_H_
