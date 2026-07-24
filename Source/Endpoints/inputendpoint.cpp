/*++

Copyright (c) Xelph. All Rights Reserved.

Module Name:

    inputendpoint.cpp

Abstract:

    Implementation of CMicArrayMiniportTopology, the microphone's topology
    filter. Beyond the generic topology properties handled by CTopologyBase,
    this reports the simulated mic array geometry (KSPROPERTY_AUDIO_MIC_ARRAY_
    GEOMETRY), fixed SNR/sensitivity values, and jack description for the
    (always "connected") integrated microphone.

Contact:

    Discord: xelphh
    Website: https://Xelph.lol

--*/

#pragma warning (disable : 4127)

#include "shared.h"
#include "pinmap.h"
#include "devicecontext.h"
#include "properties.h"
#include "inputendpoint.h"
#include "inputtopologytables.h"

constexpr float MICARRAY_SENSITIVITY = -46.5f;
constexpr float MICARRAY_SENSITIVITY2 = -23.5f;
constexpr float MICARRAY_SNR = 66.0f;

constexpr inline LONG FloatToFixedPoint16_16(float fl)
{
    return (static_cast<LONG>((fl) * (65536.0f)));
}

#pragma code_seg("PAGE")
NTSTATUS
CreateMicArrayMiniportTopology
(
    _Out_           PUNKNOWN* Unknown,
    _In_            REFCLSID,
    _In_opt_        PUNKNOWN                                UnknownOuter,
    _In_            POOL_FLAGS                              PoolFlags,
    _In_            PUNKNOWN                                UnknownAdapter,
    _In_opt_        PVOID                                   DeviceContext,
    _In_            PENDPOINT_MINIPAIR                      MiniportPair
)
{
    PAGED_CODE();

    ASSERT(Unknown);
    ASSERT(MiniportPair);

    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(DeviceContext);

    CMicArrayMiniportTopology* obj =
        new (PoolFlags, MINTOPORT_POOLTAG)
        CMicArrayMiniportTopology(UnknownOuter,
            MiniportPair->TopoDescriptor,
            MiniportPair->DeviceMaxChannels,
            MiniportPair->DeviceType);
    if (NULL == obj)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    obj->AddRef();

    *Unknown = reinterpret_cast<IUnknown*>(obj);

    return STATUS_SUCCESS;
} // CreateMicArrayMiniportTopology

CMicArrayMiniportTopology::~CMicArrayMiniportTopology
(
    void
)
{
    PAGED_CODE();

    DPF_ENTER(("[CMicArrayMiniportTopology::~CMicArrayMiniportTopology]"));
} // ~CMicArrayMiniportTopology

NTSTATUS
CMicArrayMiniportTopology::DataRangeIntersection
(
    _In_        ULONG                   PinId,
    _In_        PKSDATARANGE            ClientDataRange,
    _In_        PKSDATARANGE            MyDataRange,
    _In_        ULONG                   OutputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength)
    PVOID                   ResultantFormat     OPTIONAL,
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

STDMETHODIMP
CMicArrayMiniportTopology::GetDescription
(
    _Out_ PPCFILTER_DESCRIPTOR* OutFilterDescriptor
)
{
    PAGED_CODE();

    ASSERT(OutFilterDescriptor);

    return CTopologyBase::GetDescription(OutFilterDescriptor);
} // GetDescription

// Callers must be at IRQL PASSIVE_LEVEL.
STDMETHODIMP
CMicArrayMiniportTopology::Init
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

    DPF_ENTER(("[CMicArrayMiniportTopology::Init]"));

    NTSTATUS                    ntStatus;

    ntStatus =
        CTopologyBase::Init
        (
            UnknownAdapter,
            Port_
        );

    return ntStatus;
} // Init

STDMETHODIMP
CMicArrayMiniportTopology::NonDelegatingQueryInterface
(
    _In_         REFIID                  Interface,
    _COM_Outptr_ PVOID* Object
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

// Handles (KSPROPSETID_Audio, KSPROPERTY_AUDIO_MIC_ARRAY_GEOMETRY).
NTSTATUS
CMicArrayMiniportTopology::PropertyHandlerMicArrayGeometry
(
    _In_ PPCPROPERTY_REQUEST      PropertyRequest
)
{
    PAGED_CODE();

    ASSERT(PropertyRequest);

    DPF_ENTER(("[PropertyHandlerMicArrayGeometry]"));

    NTSTATUS    ntStatus = STATUS_INVALID_DEVICE_REQUEST;
    ULONG       nPinId = (ULONG)-1;

    if (PropertyRequest->InstanceSize >= sizeof(ULONG))
    {
        nPinId = *(PULONG(PropertyRequest->Instance));

        if (nPinId == KSPIN_TOPO_MIC_ELEMENTS)
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
                ULONG cElements = 2;
                ULONG cbNeeded = FIELD_OFFSET(KSAUDIO_MIC_ARRAY_GEOMETRY, KsMicCoord) +
                    cElements * sizeof(KSAUDIO_MICROPHONE_COORDINATES);

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
                        PKSAUDIO_MIC_ARRAY_GEOMETRY pMAG = (PKSAUDIO_MIC_ARRAY_GEOMETRY)PropertyRequest->Value;
                        const SHORT MicArray_45_Degrees = 7854;  // 10000 * pi / 4

                        // fill in mic array geometry fields
                        pMAG->usVersion = 0x0100;           // Version of Mic array specification (0x0100)
                        pMAG->usMicArrayType = (USHORT)KSMICARRAY_MICARRAYTYPE_LINEAR;        // Type of Mic Array
                        pMAG->wVerticalAngleBegin = -MicArray_45_Degrees; // Work Volume Vertical Angle Begin
                        pMAG->wVerticalAngleEnd = MicArray_45_Degrees; // Work Volume Vertical Angle End
                        pMAG->wHorizontalAngleBegin = 0;    // Work Volume HorizontalAngle Begin
                        pMAG->wHorizontalAngleEnd = 0;    // Work Volume HorizontalAngle End
                        pMAG->usFrequencyBandLo = 100;      // Low end of Freq Range
                        pMAG->usFrequencyBandHi = 8000;     // High end of Freq Range

                        pMAG->usNumberOfMicrophones = 2;    // Count of microphone coordinate structures to follow.

                        pMAG->KsMicCoord[0].usType = (USHORT)KSMICARRAY_MICTYPE_CARDIOID;
                        pMAG->KsMicCoord[0].wXCoord = 0;
                        pMAG->KsMicCoord[0].wYCoord = 100;
                        pMAG->KsMicCoord[0].wZCoord = 0;
                        pMAG->KsMicCoord[0].wVerticalAngle = 0;
                        pMAG->KsMicCoord[0].wHorizontalAngle = 0;

                        pMAG->KsMicCoord[1].usType = (USHORT)KSMICARRAY_MICTYPE_CARDIOID;
                        pMAG->KsMicCoord[1].wXCoord = 0;
                        pMAG->KsMicCoord[1].wYCoord = -100;
                        pMAG->KsMicCoord[1].wZCoord = 0;
                        pMAG->KsMicCoord[1].wVerticalAngle = 0;
                        pMAG->KsMicCoord[1].wHorizontalAngle = 0;
                        ntStatus = STATUS_SUCCESS;
                    }
                }
            }
        }
    }

    return ntStatus;
}

// Handles (KSPROPSETID_Audio, KSPROPERTY_AUDIO_MIC_SNR) and
// (KSPROPSETID_Audio, KSPROPERTY_AUDIO_MIC_SENSITIVITY2).
NTSTATUS
CMicArrayMiniportTopology::PropertyHandlerMicProperties
(
    _In_ PPCPROPERTY_REQUEST      PropertyRequest
)
{
    PAGED_CODE();

    ASSERT(PropertyRequest);

    DPF_ENTER(("[PropertyHandlerMicProperties]"));

    NTSTATUS    ntStatus = STATUS_INVALID_DEVICE_REQUEST;
    ULONG       nPinId = (ULONG)-1;
    KFLOATING_SAVE saveData;
    NTSTATUS fstatus;

    if (PropertyRequest->InstanceSize >= sizeof(ULONG))
    {
        nPinId = *(PULONG(PropertyRequest->Instance));

        if (nPinId == KSPIN_TOPO_MIC_ELEMENTS)
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
                ntStatus =
                    ValidatePropertyParams
                    (
                        PropertyRequest,
                        sizeof(LONG),
                        0
                    );

                if (NT_SUCCESS(ntStatus))
                {
                    if (PropertyRequest->Verb & KSPROPERTY_TYPE_GET)
                    {
                        if (PropertyRequest->PropertyItem->Id == KSPROPERTY_AUDIO_MIC_SNR)
                        {
                            LONG* micSNR = (LONG*)PropertyRequest->Value;
                            fstatus = KeSaveFloatingPointState(&saveData);
                            if (NT_SUCCESS(fstatus))
                            {
                                // Return microphone SNR information.
                                *micSNR = FloatToFixedPoint16_16(MICARRAY_SNR); // convert float dB to fixed point arithmetic
                                KeRestoreFloatingPointState(&saveData);
                            }
                            ntStatus = STATUS_SUCCESS;
                        }
                        else if (PropertyRequest->PropertyItem->Id == KSPROPERTY_AUDIO_MIC_SENSITIVITY2)
                        {
                            LONG* micSensitivity2 = (LONG*)PropertyRequest->Value;
                            fstatus = KeSaveFloatingPointState(&saveData);
                            if (NT_SUCCESS(fstatus))
                            {
                                // Return microphone SNR information.
                                *micSensitivity2 = FloatToFixedPoint16_16(MICARRAY_SENSITIVITY2); // convert float dB to fixed point arithmetic
                                KeRestoreFloatingPointState(&saveData);
                            }
                            ntStatus = STATUS_SUCCESS;
                        }
                    }
                    else
                    {
                        ntStatus = STATUS_INVALID_DEVICE_REQUEST;
                    }
                }
            }
        }
    }

    return ntStatus;
}

// Handles (KSPROPSETID_Jack, KSPROPERTY_JACK_DESCRIPTION).
NTSTATUS
CMicArrayMiniportTopology::PropertyHandlerJackDescription
(
    _In_ PPCPROPERTY_REQUEST      PropertyRequest
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

        if (nPinId == KSPIN_TOPO_MIC_ELEMENTS)
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
                        PKSJACK_DESCRIPTION pDesc = (PKSJACK_DESCRIPTION)(pMI + 1);

                        pMI->Size = cbNeeded;
                        pMI->Count = 1;

                        pDesc->ChannelMapping = 0;                // Don't specify channel mask for array mic
                        pDesc->Color = 0x00000000;       // Black.  This is an integrated device
                        pDesc->ConnectionType = eConnTypeUnknown; // Integrated.
                        pDesc->GenLocation = eGenLocPrimaryBox;
                        pDesc->GeoLocation = eGeoLocFront;
                        pDesc->PortConnection = ePortConnIntegratedDevice;
                        pDesc->IsConnected = TRUE;             // This is an integrated device, so it's always "connected"

                        ntStatus = STATUS_SUCCESS;
                    }
                }
            }
        }
    }

    return ntStatus;
}

// Handles (KSPROPSETID_Jack, KSPROPERTY_JACK_DESCRIPTION2).
NTSTATUS
CMicArrayMiniportTopology::PropertyHandlerJackDescription2
(
    _In_ PPCPROPERTY_REQUEST      PropertyRequest
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

        if (nPinId == KSPIN_TOPO_MIC_ELEMENTS)
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
                        PKSJACK_DESCRIPTION2 pDesc = (PKSJACK_DESCRIPTION2)(pMI + 1);

                        pMI->Size = cbNeeded;
                        pMI->Count = 1;

                        RtlZeroMemory(pDesc, sizeof(KSJACK_DESCRIPTION2));

                        // DeviceStateInfo: lower 16 bits indicate active/streaming/idle/hw-not-ready.
                        pDesc->DeviceStateInfo = 0;

                        // If IsConnected is TRUE but JACKDESC2_PRESENCE_DETECT_CAPABILITY is not
                        // set here, clients must treat that TRUE as "no presence detection" rather
                        // than "jack physically inserted" - see IKsJackDescription2 docs.
                        pDesc->JackCapabilities = 0;

                        ntStatus = STATUS_SUCCESS;
                    }
                }
            }
        }
    }

    return ntStatus;
}

// PortCls entry point for mic array KS properties; MajorTarget is the miniport instance.
NTSTATUS
PropertyHandler_MicArrayTopoFilter
(
    _In_ PPCPROPERTY_REQUEST      PropertyRequest
)
{
    PAGED_CODE();

    ASSERT(PropertyRequest);

    DPF_ENTER(("[PropertyHandler_MicArrayTopoFilter]"));

    NTSTATUS            ntStatus = STATUS_INVALID_DEVICE_REQUEST;
    PCMicArrayMiniportTopology  pMiniport = (PCMicArrayMiniportTopology)PropertyRequest->MajorTarget;

    if (IsEqualGUIDAligned(*PropertyRequest->PropertyItem->Set, KSPROPSETID_Audio))
    {
        if (PropertyRequest->PropertyItem->Id == KSPROPERTY_AUDIO_MIC_ARRAY_GEOMETRY)
        {
            ntStatus = pMiniport->PropertyHandlerMicArrayGeometry(PropertyRequest);
        }
        else if (PropertyRequest->PropertyItem->Id == KSPROPERTY_AUDIO_MIC_SENSITIVITY2)
        {
            ntStatus = pMiniport->PropertyHandlerMicProperties(PropertyRequest);
        }
        else if (PropertyRequest->PropertyItem->Id == KSPROPERTY_AUDIO_MIC_SNR)
        {
            ntStatus = pMiniport->PropertyHandlerMicProperties(PropertyRequest);
        }
    }
    else if (IsEqualGUIDAligned(*PropertyRequest->PropertyItem->Set, KSPROPSETID_Jack))
    {
        if (PropertyRequest->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION)
        {
            ntStatus = pMiniport->PropertyHandlerJackDescription(PropertyRequest);
        }
        else if (PropertyRequest->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION2)
        {
            ntStatus = pMiniport->PropertyHandlerJackDescription2(PropertyRequest);
        }
    }

    return ntStatus;
} // PropertyHandler_TopoFilter

NTSTATUS
PropertyHandler_MicArrayTopology
(
    _In_ PPCPROPERTY_REQUEST      PropertyRequest
)
{
    PAGED_CODE();

    ASSERT(PropertyRequest);

    DPF_ENTER(("[PropertyHandler_MicArrayTopology]"));

    PCMicArrayMiniportTopology pMiniport = (PCMicArrayMiniportTopology)PropertyRequest->MajorTarget;

    return pMiniport->PropertyHandlerGeneric(PropertyRequest);
} // PropertyHandler_MicArrayTopology

#pragma code_seg()
