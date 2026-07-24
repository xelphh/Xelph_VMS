/*++

Copyright (c) Xelph. All Rights Reserved.

Module Name:

    outputtopology.cpp

Abstract:

    Speaker-side topology miniport (CMiniportTopology). Thin wrapper around
    CTopologyBase that supplies the factory function used by the endpoint
    table (CreateTopologyBase), the jack-description property handlers
    (KSPROPERTY_JACK_DESCRIPTION / DESCRIPTION2, used by Windows to show
    jack presence/type in the sound control panel), and QueryInterface for
    this filter's COM identity.

Contact:

    Discord: xelphh
    Website: https://Xelph.lol

--*/

#pragma warning (disable : 4127)

#include "shared.h"
#include "pinmap.h"
#include "waveengine.h"
#include "outputtopology.h"

#pragma code_seg("PAGE")
NTSTATUS
CreateTopologyBase
(
    _Out_           PUNKNOWN *                              Unknown,
    _In_            REFCLSID,
    _In_opt_        PUNKNOWN                                UnknownOuter,
    _In_            POOL_FLAGS                              PoolFlags,
    _In_            PUNKNOWN                                UnknownAdapter,
    _In_opt_        PVOID                                   DeviceContext,
    _In_            PENDPOINT_MINIPAIR                      MiniportPair
)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(UnknownAdapter);

    ASSERT(Unknown);
    ASSERT(MiniportPair);

    CMiniportTopology *obj =
        new (PoolFlags, MINWAVERT_POOLTAG)
            CMiniportTopology( UnknownOuter,
                               MiniportPair->TopoDescriptor,
                               MiniportPair->DeviceMaxChannels,
                               MiniportPair->DeviceType,
                               DeviceContext );
    if (NULL == obj)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    obj->AddRef();
    *Unknown = reinterpret_cast<IUnknown*>(obj);

    return STATUS_SUCCESS;
} // CreateTopologyBase

#pragma code_seg("PAGE")
CMiniportTopology::~CMiniportTopology
(
    void
)
{
    PAGED_CODE();

    DPF_ENTER(("[CMiniportTopology::~CMiniportTopology]"));
} // ~CMiniportTopology

#pragma code_seg("PAGE")
NTSTATUS
CMiniportTopology::DataRangeIntersection
(
    _In_        ULONG                   PinId,
    _In_        PKSDATARANGE            ClientDataRange,
    _In_        PKSDATARANGE            MyDataRange,
    _In_        ULONG                   OutputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength)
                PVOID                   ResultantFormat,
    _Out_       PULONG                  ResultantFormatLength
)
{
    PAGED_CODE();

    return
        CTopologyBase::DataRangeIntersection
        (
            PinId,
            ClientDataRange,
            MyDataRange,
            OutputBufferLength,
            ResultantFormat,
            ResultantFormatLength
        );
} // DataRangeIntersection

#pragma code_seg("PAGE")
STDMETHODIMP
CMiniportTopology::GetDescription
(
    _Out_ PPCFILTER_DESCRIPTOR *  OutFilterDescriptor
)
{
    PAGED_CODE();

    ASSERT(OutFilterDescriptor);

    return CTopologyBase::GetDescription(OutFilterDescriptor);
} // GetDescription

// Callers must be at IRQL PASSIVE_LEVEL.
#pragma code_seg("PAGE")
STDMETHODIMP
CMiniportTopology::Init
(
    _In_ PUNKNOWN                 UnknownAdapter,
    _In_ PRESOURCELIST            ResourceList,
    _In_ PPORTTOPOLOGY            Port_
)
{
    UNREFERENCED_PARAMETER(ResourceList);

    PAGED_CODE();

    ASSERT(UnknownAdapter);
    ASSERT(Port_);

    DPF_ENTER(("[CMiniportTopology::Init]"));

    NTSTATUS                    ntStatus;

    ntStatus =
        CTopologyBase::Init
        (
            UnknownAdapter,
            Port_
        );

    IF_FAILED_ACTION_JUMP(
        ntStatus,
        DPF(D_ERROR, ("Init: CTopologyBase::Init failed, 0x%x", ntStatus)),
        Done);

Done:
    return ntStatus;
} // Init

#pragma code_seg("PAGE")
STDMETHODIMP
CMiniportTopology::NonDelegatingQueryInterface
(
    _In_ REFIID                  Interface,
    _COM_Outptr_ PVOID      * Object
)
{
    PAGED_CODE();

    ASSERT(Object);

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = PVOID(PUNKNOWN(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniport))
    {
        *Object = PVOID(PMINIPORT(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportTopology))
    {
        *Object = PVOID(PMINIPORTTOPOLOGY(this));
    }
    else
    {
        *Object = NULL;
    }

    if (*Object)
    {
        // We reference the interface for the caller.
        PUNKNOWN(*Object)->AddRef();
        return(STATUS_SUCCESS);
    }

    return(STATUS_INVALID_PARAMETER);
} // NonDelegatingQueryInterface

// Handles (KSPROPSETID_Jack, KSPROPERTY_JACK_DESCRIPTION).
#pragma code_seg("PAGE")
NTSTATUS
CMiniportTopology::PropertyHandlerJackDescription
(
    _In_        PPCPROPERTY_REQUEST                      PropertyRequest,
    _In_        ULONG                                    cJackDescriptions,
    _In_reads_(cJackDescriptions) PKSJACK_DESCRIPTION *  JackDescriptions
)
{
    PAGED_CODE();

    ASSERT(PropertyRequest);

    DPF_ENTER(("[PropertyHandlerJackDescription]"));

    NTSTATUS ntStatus = STATUS_INVALID_DEVICE_REQUEST;
    ULONG    nPinId = (ULONG)-1;

    if (PropertyRequest->InstanceSize >= sizeof(ULONG))
    {
        nPinId = *(PULONG(PropertyRequest->Instance));

        if ((nPinId < cJackDescriptions) && (JackDescriptions[nPinId] != NULL))
        {
            if (PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
            {
                ntStatus =
                    PropertyHandler_BasicSupport
                    (
                        PropertyRequest,
                        KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET,
                        VT_ILLEGAL
                    );
            }
            else
            {
                ULONG cbNeeded = sizeof(KSMULTIPLE_ITEM) + sizeof(KSJACK_DESCRIPTION);

                if (PropertyRequest->ValueSize == 0)
                {
                    PropertyRequest->ValueSize = cbNeeded;
                    ntStatus = STATUS_BUFFER_OVERFLOW;
                }
                else if (PropertyRequest->ValueSize < cbNeeded)
                {
                    ntStatus = STATUS_BUFFER_TOO_SMALL;
                }
                else
                {
                    if (PropertyRequest->Verb & KSPROPERTY_TYPE_GET)
                    {
                        PKSMULTIPLE_ITEM pMI = (PKSMULTIPLE_ITEM)PropertyRequest->Value;
                        PKSJACK_DESCRIPTION pDesc = (PKSJACK_DESCRIPTION)(pMI+1);

                        pMI->Size = cbNeeded;
                        pMI->Count = 1;

                        RtlCopyMemory(pDesc, JackDescriptions[nPinId], sizeof(KSJACK_DESCRIPTION));
                        ntStatus = STATUS_SUCCESS;
                    }
                }
            }
        }
    }

    return ntStatus;
}

// Handles (KSPROPSETID_Jack, KSPROPERTY_JACK_DESCRIPTION2).
#pragma code_seg("PAGE")
NTSTATUS
CMiniportTopology::PropertyHandlerJackDescription2
(
    _In_        PPCPROPERTY_REQUEST                      PropertyRequest,
    _In_        ULONG                                    cJackDescriptions,
    _In_reads_(cJackDescriptions) PKSJACK_DESCRIPTION *  JackDescriptions,
    _In_        DWORD                                    JackCapabilities
)
{
    PAGED_CODE();

    ASSERT(PropertyRequest);

    DPF_ENTER(("[PropertyHandlerJackDescription2]"));

    NTSTATUS ntStatus = STATUS_INVALID_DEVICE_REQUEST;
    ULONG    nPinId = (ULONG)-1;

    if (PropertyRequest->InstanceSize >= sizeof(ULONG))
    {
        nPinId = *(PULONG(PropertyRequest->Instance));

        if ((nPinId < cJackDescriptions) && (JackDescriptions[nPinId] != NULL))
        {
            if (PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
            {
                ntStatus =
                    PropertyHandler_BasicSupport
                    (
                        PropertyRequest,
                        KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET,
                        VT_ILLEGAL
                    );
            }
            else
            {
                ULONG cbNeeded = sizeof(KSMULTIPLE_ITEM) + sizeof(KSJACK_DESCRIPTION2);

                if (PropertyRequest->ValueSize == 0)
                {
                    PropertyRequest->ValueSize = cbNeeded;
                    ntStatus = STATUS_BUFFER_OVERFLOW;
                }
                else if (PropertyRequest->ValueSize < cbNeeded)
                {
                    ntStatus = STATUS_BUFFER_TOO_SMALL;
                }
                else
                {
                    if (PropertyRequest->Verb & KSPROPERTY_TYPE_GET)
                    {
                        PKSMULTIPLE_ITEM pMI = (PKSMULTIPLE_ITEM)PropertyRequest->Value;
                        PKSJACK_DESCRIPTION2 pDesc = (PKSJACK_DESCRIPTION2)(pMI+1);

                        pMI->Size = cbNeeded;
                        pMI->Count = 1;

                        RtlZeroMemory(pDesc, sizeof(KSJACK_DESCRIPTION2));

                        // DeviceStateInfo: lower 16 bits indicate active/streaming/idle/hw-not-ready.
                        pDesc->DeviceStateInfo = 0;

                        // If IsConnected is TRUE but JACKDESC2_PRESENCE_DETECT_CAPABILITY is not
                        // set here, clients must treat that TRUE as "no presence detection" rather
                        // than "jack physically inserted" - see IKsJackDescription2 docs.
                        pDesc->JackCapabilities = JackCapabilities;

                        ntStatus = STATUS_SUCCESS;
                    }
                }
            }
        }
    }

    return ntStatus;
}

// PortCls entry point for topology property requests; MajorTarget is the miniport instance.
#pragma code_seg("PAGE")
NTSTATUS
PropertyHandler_Topology
(
    _In_ PPCPROPERTY_REQUEST      PropertyRequest
)
{
    PAGED_CODE();

    ASSERT(PropertyRequest);

    DPF_ENTER(("[PropertyHandler_Topology]"));

    return
        ((PCMiniportTopology)
        (PropertyRequest->MajorTarget))->PropertyHandlerGeneric
                    (
                        PropertyRequest
                    );
} // PropertyHandler_Topology
