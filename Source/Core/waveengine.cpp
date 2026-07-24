/*++

Copyright (c) Xelph. All Rights Reserved.

Module Name:

    waveengine.cpp

Abstract:

    Implementation of CMiniportWaveRT, the WaveRT miniport - the core
    streaming engine for the speaker and microphone endpoints. Handles
    filter-level setup (CreateWaveEngine/Init), format negotiation and
    validation (DataRangeIntersection, IsFormatSupported, the proposed
    format property handlers), pin instance bookkeeping (ValidateStreamCreate/
    StreamCreated/StreamClosed, the system/loopback/offload stream limits),
    DRM rights mixing across active streams, and stream creation (NewStream,
    which hands off to CMiniportWaveRTStream in streamworker.cpp).

Contact:

    Discord: xelphh
    Website: https://Xelph.lol

--*/

#pragma warning (disable : 4127)

#include "shared.h"
#include <limits.h>
#include "pinmap.h"
#include "waveengine.h"
#include "streamworker.h"
#include "inputformats.h"

#define EFFECTS_LIST_COUNT 2

#pragma code_seg("PAGE")
NTSTATUS
CreateWaveEngine
(
    _Out_           PUNKNOWN                              * Unknown,
    _In_            REFCLSID,
    _In_opt_        PUNKNOWN                                UnknownOuter,
    _In_            POOL_FLAGS                              PoolFlags,
    _In_            PUNKNOWN                                UnknownAdapter,
    _In_opt_        PVOID                                   DeviceContext,
    _In_            PENDPOINT_MINIPAIR                      MiniportPair
)
{
    UNREFERENCED_PARAMETER(UnknownOuter);

    PAGED_CODE();

    ASSERT(Unknown);
    ASSERT(MiniportPair);

    CMiniportWaveRT *obj = new (PoolFlags, MINWAVERT_POOLTAG) CMiniportWaveRT
                                                             (
                                                                UnknownAdapter,
                                                                MiniportPair,
                                                                DeviceContext
                                                             );
    if (NULL == obj)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    obj->AddRef();
    *Unknown = reinterpret_cast<IUnknown*>(obj);

    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
CMiniportWaveRT::~CMiniportWaveRT
(
    void
)
{
    PAGED_CODE();

    DPF_ENTER(("[CMiniportWaveRT::~CMiniportWaveRT]"));

    if (m_pDeviceFormat)
    {
        ExFreePoolWithTag( m_pDeviceFormat, MINWAVERT_POOLTAG );
        m_pDeviceFormat = NULL;
    }

    if (m_pMixFormat)
    {
        ExFreePoolWithTag( m_pMixFormat, MINWAVERT_POOLTAG );
        m_pMixFormat = NULL;
    }

    if (m_pbMuted)
    {
        ExFreePoolWithTag(m_pbMuted, MINWAVERT_POOLTAG);
        m_pbMuted = NULL;
    }

    if (m_plVolumeLevel)
    {
        ExFreePoolWithTag(m_plVolumeLevel, MINWAVERT_POOLTAG);
        m_plVolumeLevel = NULL;
    }

    if (m_pDrmPort)
    {
        m_pDrmPort->Release();
        m_pDrmPort = NULL;
    }

    if (m_pPortEvents)
    {
        m_pPortEvents->Release();
        m_pPortEvents = NULL;
    }

    if (m_SystemStreams)
    {
        ExFreePoolWithTag( m_SystemStreams, MINWAVERT_POOLTAG );
        m_SystemStreams = NULL;
    }

} // ~CMiniportWaveRT

// Determines the highest quality intersection of two data ranges. For the
// mic array pin this endpoint has exactly one supported format, so that
// format is returned directly; every other pin defers to the class handler.
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::DataRangeIntersection
(
    _In_        ULONG                       PinId,
    _In_        PKSDATARANGE                ClientDataRange,
    _In_        PKSDATARANGE                MyDataRange,
    _In_        ULONG                       OutputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength)
                PVOID                       ResultantFormat,
    _Out_       PULONG                      ResultantFormatLength
)
{
    UNREFERENCED_PARAMETER(PinId);

    ULONG                   requiredSize;

    PAGED_CODE();

    if (!IsEqualGUIDAligned(ClientDataRange->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX))
    {
        return STATUS_NOT_IMPLEMENTED;
    }

    if ((this->m_DeviceType) == eMicArrayDevice1)
    {
        requiredSize = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);

        // If the request is only for the size of the resultant structure,
        // return it now before checking for other errors.
        if (!OutputBufferLength)
        {
            *ResultantFormatLength = requiredSize;
            return STATUS_BUFFER_OVERFLOW;
        }
        else if (OutputBufferLength < requiredSize)
        {
            return STATUS_BUFFER_TOO_SMALL;
        }

        PKSDATAFORMAT_WAVEFORMATEXTENSIBLE resultantFormat;
        resultantFormat = (PKSDATAFORMAT_WAVEFORMATEXTENSIBLE)ResultantFormat;
        *resultantFormat = *MicArrayPinSupportedDeviceFormats;
        *ResultantFormatLength = requiredSize;

        return STATUS_SUCCESS;
    }
    else
    {
        requiredSize = sizeof(KSDATAFORMAT_WAVEFORMATEX);

        if (!OutputBufferLength)
        {
            *ResultantFormatLength = requiredSize;
            return STATUS_BUFFER_OVERFLOW;
        }
        else if (OutputBufferLength < requiredSize)
        {
            return STATUS_BUFFER_TOO_SMALL;
        }

        // Verify channel count is supported. This routine assumes a separate data
        // range for each supported channel count.
        if (((PKSDATARANGE_AUDIO)MyDataRange)->MaximumChannels != ((PKSDATARANGE_AUDIO)ClientDataRange)->MaximumChannels)
        {
            return STATUS_NO_MATCH;
        }

        // Let the class handler do the rest.
        return STATUS_NOT_IMPLEMENTED;
    }

} // DataRangeIntersection

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::GetDescription
(
    _Out_ PPCFILTER_DESCRIPTOR * OutFilterDescriptor
)
{
    PAGED_CODE();

    ASSERT(OutFilterDescriptor);

    *OutFilterDescriptor = &m_FilterDesc;

    return STATUS_SUCCESS;
} // GetDescription

// Callers must be at IRQL PASSIVE_LEVEL.
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::Init
(
    _In_  PUNKNOWN                UnknownAdapter_,
    _In_  PRESOURCELIST           ResourceList_,
    _In_  PPORTWAVERT             Port_
)
{
    UNREFERENCED_PARAMETER(UnknownAdapter_);
    UNREFERENCED_PARAMETER(ResourceList_);
    UNREFERENCED_PARAMETER(Port_);
    PAGED_CODE();

    ASSERT(UnknownAdapter_);
    ASSERT(Port_);

    DPF_ENTER(("[CMiniportWaveRT::Init]"));

    NTSTATUS ntStatus = STATUS_SUCCESS;
    size_t   size;

    m_ulSystemAllocated                 = 0;
    m_SystemStreams                     = NULL;
    m_bGfxEnabled                       = FALSE;
    m_pMixFormat                        = NULL;
    m_pDeviceFormat                     = NULL;
    m_ulMixDrmContentId                 = 0;
    m_pbMuted = NULL;
    m_plVolumeLevel = NULL;
    RtlZeroMemory(&m_MixDrmRights, sizeof(m_MixDrmRights));

    if (IsRenderDevice())
    {
        if (m_ulMaxSystemStreams == 0 )
        {
            return STATUS_INVALID_DEVICE_STATE;
        }

        size = sizeof(PCMiniportWaveRTStream) * m_ulMaxSystemStreams;
        m_SystemStreams = (PCMiniportWaveRTStream *)ExAllocatePool2(POOL_FLAG_NON_PAGED, size, MINWAVERT_POOLTAG);
        if (m_SystemStreams == NULL)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        if (!NT_SUCCESS(Port_->QueryInterface(IID_IDrmPort2, (PVOID *)&m_pDrmPort)))
        {
            m_pDrmPort = NULL;
        }
    }

    if (!NT_SUCCESS(Port_->QueryInterface(IID_IPortEvents, (PVOID *)&m_pPortEvents)))
    {
        m_pPortEvents = NULL;
    }

    return ntStatus;
} // Init

// Creates a new logical stream on the given pin. Callers must be at IRQL PASSIVE_LEVEL.
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::NewStream
(
    _Out_ PMINIPORTWAVERTSTREAM * OutStream,
    _In_  PPORTWAVERTSTREAM       OuterUnknown,
    _In_  ULONG                   Pin,
    _In_  BOOLEAN                 Capture,
    _In_  PKSDATAFORMAT           DataFormat
)
{
    PAGED_CODE();

    ASSERT(OutStream);
    ASSERT(DataFormat);

    DPF_ENTER(("[CMiniportWaveRT::NewStream]"));

    NTSTATUS                    ntStatus = STATUS_SUCCESS;
    PCMiniportWaveRTStream      stream = NULL;
    GUID                        signalProcessingMode = AUDIO_SIGNALPROCESSINGMODE_DEFAULT;

    *OutStream = NULL;

    if ( DataFormat->Flags & KSDATAFORMAT_ATTRIBUTES )
    {
        // The attributes are aligned (QWORD alignment) after the data format.
        PKSMULTIPLE_ITEM attributes = (PKSMULTIPLE_ITEM) (((PBYTE)DataFormat) + ((DataFormat->FormatSize + FILE_QUAD_ALIGNMENT) & ~FILE_QUAD_ALIGNMENT));
        ntStatus = GetAttributesFromAttributeList(attributes, attributes->Size, &signalProcessingMode);
    }

    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = ValidateStreamCreate(Pin, Capture);
    }

    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = IsFormatSupported(Pin, Capture, DataFormat);
    }

    if (NT_SUCCESS(ntStatus))
    {
        // Stream must be in NonPagedPool(Nx) because of file saving.
        stream = new (POOL_FLAG_NON_PAGED, MINWAVERT_POOLTAG)
            CMiniportWaveRTStream(NULL);

        if (stream)
        {
            stream->AddRef();

            ntStatus =
                stream->Init
                (
                    this,
                    OuterUnknown,
                    Pin,
                    Capture,
                    DataFormat,
                    signalProcessingMode
                );
        }
        else
        {
            ntStatus = STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    if (NT_SUCCESS(ntStatus))
    {
        *OutStream = PMINIPORTWAVERTSTREAM(stream);
        (*OutStream)->AddRef();

        // The stream now has references for the caller, who expects them to be there.
    }

    // This is our own private reference; the caller has its own, so we can
    // release ours regardless of outcome.
    if (stream)
    {
        stream->Release();
    }

    return ntStatus;
} // NewStream

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRT::NonDelegatingQueryInterface
(
    _In_ REFIID  Interface,
    _COM_Outptr_ PVOID * Object
)
{
    PAGED_CODE();

    ASSERT(Object);

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = PVOID(PUNKNOWN(PMINIPORTWAVERT(this)));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniport))
    {
        *Object = PVOID(PMINIPORT(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRT))
    {
        *Object = PVOID(PMINIPORTWAVERT(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportAudioSignalProcessing))
    {
        *Object = PVOID(PMINIPORTAudioSignalProcessing(this));
    }
    else
    {
        *Object = NULL;
    }

    if (*Object)
    {
        // We reference the interface for the caller.
        PUNKNOWN(*Object)->AddRef();
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_PARAMETER;
} // NonDelegatingQueryInterface

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS) CMiniportWaveRT::GetDeviceDescription(_Out_ PDEVICE_DESCRIPTION DmaDeviceDescription)
{
    PAGED_CODE ();

    ASSERT (DmaDeviceDescription);

    DPF_ENTER(("[CMiniportWaveRT::GetDeviceDescription]"));

    RtlZeroMemory (DmaDeviceDescription, sizeof (DEVICE_DESCRIPTION));

    // Same description is used for every m_DeviceType.
    DmaDeviceDescription->Master = TRUE;
    DmaDeviceDescription->ScatterGather = TRUE;
    DmaDeviceDescription->Dma32BitAddresses = TRUE;
    DmaDeviceDescription->InterfaceType = PCIBus;
    DmaDeviceDescription->MaximumLength = 0xFFFFFFFF;

    return STATUS_SUCCESS;
}

// Returns STATUS_INVALID_PARAMETER for an out-of-range pin, STATUS_SUCCESS
// with the mode list filled in if Pin supports one or more signal processing
// modes, or STATUS_NOT_SUPPORTED for pins that don't (e.g. bridge pins).
#pragma code_seg("PAGE")
NTSTATUS
CMiniportWaveRT::GetModes
(
    _In_                                        ULONG     Pin,
    _Out_writes_opt_(*NumSignalProcessingModes) GUID*     SignalProcessingModes,
    _Inout_                                     ULONG*    NumSignalProcessingModes
)
{
    PAGED_CODE();

    DPF_ENTER(("[CMiniportWaveRT::GetModes]"));

    NTSTATUS                ntStatus    = STATUS_INVALID_PARAMETER;
    ULONG                   numModes    = 0;
    MODE_AND_DEFAULT_FORMAT *modeInfo   = NULL;

    if (Pin >= m_pMiniportPair->WaveDescriptor->PinCount)
    {
        return STATUS_INVALID_PARAMETER;
    }

    // Valid only on: sink+offload pins for offload-capable render, sink pin
    // for non-offload-capable render, or the source pin for capture.
    numModes = GetPinSupportedDeviceModes(Pin, &modeInfo);
    if (numModes == 0)
    {
        return STATUS_NOT_SUPPORTED;
    }

    if (SignalProcessingModes != NULL)
    {
        if (*NumSignalProcessingModes < numModes)
        {
            *NumSignalProcessingModes = numModes;
            ntStatus = STATUS_BUFFER_TOO_SMALL;
            goto Done;
        }

        for (ULONG i=0; i<numModes; ++i)
        {
            SignalProcessingModes[i] = modeInfo[i].Mode;
        }
    }

    ASSERT(numModes > 0);
    *NumSignalProcessingModes = numModes;
    ntStatus = STATUS_SUCCESS;

Done:
    return ntStatus;
}

#pragma code_seg("PAGE")
NTSTATUS
CMiniportWaveRT::ValidateStreamCreate
(
    _In_    ULONG   _Pin,
    _In_    BOOLEAN _Capture
)
{
    PAGED_CODE();

    DPF_ENTER(("[CMiniportWaveRT::ValidateStreamCreate]"));

    NTSTATUS ntStatus = STATUS_NOT_SUPPORTED;

    if (_Capture)
    {
        if (IsSystemCapturePin(_Pin))
        {
            VERIFY_PIN_INSTANCE_RESOURCES_AVAILABLE(ntStatus, m_ulSystemAllocated, m_ulMaxSystemStreams);
        }
    }
    else
    {
        if (IsSystemRenderPin(_Pin))
        {
            VERIFY_PIN_INSTANCE_RESOURCES_AVAILABLE(ntStatus, m_ulSystemAllocated, m_ulMaxSystemStreams);
        }
    }

    return ntStatus;
}

#pragma code_seg()
_Use_decl_annotations_
VOID CMiniportWaveRT::AcquireFormatsAndModesLock()
{
    KeAcquireSpinLock(&m_DeviceFormatsAndModesLock, &m_DeviceFormatsAndModesIrql);
}

#pragma code_seg()
_Use_decl_annotations_
VOID CMiniportWaveRT::ReleaseFormatsAndModesLock()
{
    KeReleaseSpinLock(&m_DeviceFormatsAndModesLock, m_DeviceFormatsAndModesIrql);
}

// Returns the KSDATAFORMAT_WAVEFORMATEXTENSIBLE array supported by PinId; the
// index order matches the filter's pin descriptor list.
#pragma code_seg()
_Use_decl_annotations_
ULONG CMiniportWaveRT::GetPinSupportedDeviceFormats(_In_ ULONG PinId, _Outptr_opt_result_buffer_(return) KSDATAFORMAT_WAVEFORMATEXTENSIBLE **ppFormats)
{
    PPIN_DEVICE_FORMATS_AND_MODES pDeviceFormatsAndModes = NULL;

    AcquireFormatsAndModesLock();

    pDeviceFormatsAndModes = m_DeviceFormatsAndModes;
    ASSERT(m_DeviceFormatsAndModesCount > PinId);
    ASSERT(pDeviceFormatsAndModes[PinId].WaveFormats != NULL);
    ASSERT(pDeviceFormatsAndModes[PinId].WaveFormatsCount > 0);

    if (ppFormats != NULL)
    {
        *ppFormats = pDeviceFormatsAndModes[PinId].WaveFormats;
    }

    ReleaseFormatsAndModesLock();

    return pDeviceFormatsAndModes[PinId].WaveFormatsCount;
}

// Returns the MODE_AND_DEFAULT_FORMAT array supported by PinId, or 0 if none;
// the index order matches the filter's pin descriptor list.
#pragma code_seg()
_Use_decl_annotations_
ULONG CMiniportWaveRT::GetPinSupportedDeviceModes(_In_ ULONG PinId, _Outptr_opt_result_buffer_(return) _On_failure_(_Deref_post_null_) MODE_AND_DEFAULT_FORMAT **ppModes)
{
    PMODE_AND_DEFAULT_FORMAT modes;
    ULONG numModes;

    AcquireFormatsAndModesLock();

    ASSERT(m_DeviceFormatsAndModesCount > PinId);
    ASSERT((m_DeviceFormatsAndModes[PinId].ModeAndDefaultFormatCount == 0) == (m_DeviceFormatsAndModes[PinId].ModeAndDefaultFormat == NULL));

    modes = m_DeviceFormatsAndModes[PinId].ModeAndDefaultFormat;
    numModes = m_DeviceFormatsAndModes[PinId].ModeAndDefaultFormatCount;

    if (ppModes != NULL)
    {
        if (numModes > 0)
        {
            *ppModes = modes;
        }
        else
        {
            // SAL requires this to be NULL on failure; OACR can't otherwise prove it.
            *ppModes = NULL;
        }
    }

    ReleaseFormatsAndModesLock();
    return numModes;
}

#pragma code_seg()
BOOL CMiniportWaveRT::IsSystemCapturePin(ULONG nPinId)
{
    AcquireFormatsAndModesLock();

    PINTYPE pinType = m_DeviceFormatsAndModes[nPinId].PinType;

    ReleaseFormatsAndModesLock();
    return (pinType == SystemCapturePin);
}

#pragma code_seg()
BOOL CMiniportWaveRT::IsSystemRenderPin(ULONG nPinId)
{
    AcquireFormatsAndModesLock();

    PINTYPE pinType = m_DeviceFormatsAndModes[nPinId].PinType;

    ReleaseFormatsAndModesLock();
    return (pinType == SystemRenderPin);
}

#pragma code_seg()
BOOL CMiniportWaveRT::IsBridgePin(ULONG nPinId)
{
    AcquireFormatsAndModesLock();

    PINTYPE pinType = m_DeviceFormatsAndModes[nPinId].PinType;

    ReleaseFormatsAndModesLock();
    return (pinType == BridgePin);
}

#pragma code_seg("PAGE")
NTSTATUS
CMiniportWaveRT::StreamCreated
(
    _In_ ULONG                  _Pin,
    _In_ PCMiniportWaveRTStream _Stream
)
{
    PAGED_CODE();

    PCMiniportWaveRTStream * streams        = NULL;
    ULONG                    count          = 0;

    DPF_ENTER(("[CMiniportWaveRT::StreamCreated]"));

    if (IsSystemCapturePin(_Pin))
    {
        ALLOCATE_PIN_INSTANCE_RESOURCES(m_ulSystemAllocated);
        return STATUS_SUCCESS;
    }
    else if (IsSystemRenderPin(_Pin))
    {
        ALLOCATE_PIN_INSTANCE_RESOURCES(m_ulSystemAllocated);
        streams = m_SystemStreams;
        count = m_ulMaxSystemStreams;
    }

    if (streams != NULL)
    {
        ULONG i = 0;
        for (; i<count; ++i)
        {
            if (streams[i] == NULL)
            {
                streams[i] = _Stream;
                break;
            }
        }
        ASSERT(i != count);
    }

    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
NTSTATUS
CMiniportWaveRT::StreamClosed
(
    _In_ ULONG                  _Pin,
    _In_ PCMiniportWaveRTStream _Stream
)
{
    PAGED_CODE();

    bool                      updateDrmRights = false;
    PCMiniportWaveRTStream  * streams         = NULL;
    ULONG                     count           = 0;

    DPF_ENTER(("[CMiniportWaveRT::StreamClosed]"));

    if (IsSystemCapturePin(_Pin))
    {
        FREE_PIN_INSTANCE_RESOURCES(m_ulSystemAllocated);
        return STATUS_SUCCESS;
    }
    else if (IsSystemRenderPin(_Pin))
    {
        FREE_PIN_INSTANCE_RESOURCES(m_ulSystemAllocated);
        streams = m_SystemStreams;
        count = m_ulMaxSystemStreams;
        updateDrmRights = true;
    }

    if (streams != NULL)
    {
        ULONG i = 0;
        for (; i<count; ++i)
        {
            if (streams[i] == _Stream)
            {
                streams[i] = NULL;
                break;
            }
        }
        ASSERT(i != count);
    }

    if (updateDrmRights)
    {
        UpdateDrmRights();
    }

    return STATUS_SUCCESS;
}

// Parses a KSMULTIPLE_ITEM attribute list and extracts the signal processing
// mode attribute if present, defaulting to AUDIO_SIGNALPROCESSINGMODE_DEFAULT.
// Only KSATTRIBUTEID_AUDIOSIGNALPROCESSING_MODE is understood today; if more
// attribute types are added later this should become table-driven.
#pragma code_seg("PAGE")
NTSTATUS
CMiniportWaveRT::GetAttributesFromAttributeList
(
    _In_ const KSMULTIPLE_ITEM *_pAttributes,
    _In_ size_t _Size,
    _Out_ GUID* _pSignalProcessingMode
)
{
    PAGED_CODE();

    DPF_ENTER(("[CMiniportWaveRT::GetAttributesFromAttributeList]"));

    size_t cbRemaining = _Size;

    *_pSignalProcessingMode = AUDIO_SIGNALPROCESSINGMODE_DEFAULT;

    if (cbRemaining < sizeof(KSMULTIPLE_ITEM))
    {
        return STATUS_INVALID_PARAMETER;
    }
    cbRemaining -= sizeof(KSMULTIPLE_ITEM);

    PKSATTRIBUTE attributeHeader = (PKSATTRIBUTE)(_pAttributes + 1);

    for (ULONG i = 0; i < _pAttributes->Count; i++)
    {
        if (cbRemaining < sizeof(KSATTRIBUTE))
        {
            return STATUS_INVALID_PARAMETER;
        }

        if (attributeHeader->Attribute == KSATTRIBUTEID_AUDIOSIGNALPROCESSING_MODE)
        {
            KSATTRIBUTE_AUDIOSIGNALPROCESSING_MODE* signalProcessingModeAttribute;

            if (cbRemaining < sizeof(KSATTRIBUTE_AUDIOSIGNALPROCESSING_MODE))
            {
                return STATUS_INVALID_PARAMETER;
            }

            if (attributeHeader->Size != sizeof(KSATTRIBUTE_AUDIOSIGNALPROCESSING_MODE))
            {
                return STATUS_INVALID_PARAMETER;
            }

            signalProcessingModeAttribute = (KSATTRIBUTE_AUDIOSIGNALPROCESSING_MODE*)attributeHeader;

            *_pSignalProcessingMode = signalProcessingModeAttribute->SignalProcessingMode;
        }
        else
        {
            return STATUS_NOT_SUPPORTED;
        }

        // Advance to the next attribute (QWORD aligned).
        ULONG cbAttribute = ((attributeHeader->Size + FILE_QUAD_ALIGNMENT) & ~FILE_QUAD_ALIGNMENT);

        attributeHeader = (PKSATTRIBUTE) (((PBYTE)attributeHeader) + cbAttribute);
        cbRemaining -= cbAttribute;
    }

    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
NTSTATUS
CMiniportWaveRT::IsFormatSupported
(
    _In_ ULONG          _ulPin,
    _In_ BOOLEAN        _bCapture,
    _In_ PKSDATAFORMAT  _pDataFormat
)
{
    PAGED_CODE();

    DPF_ENTER(("[CMiniportWaveRT::IsFormatSupported]"));

    NTSTATUS                            ntStatus = STATUS_NO_MATCH;
    PKSDATAFORMAT_WAVEFORMATEXTENSIBLE  pPinFormats = NULL;
    ULONG                               cPinFormats = 0;

    UNREFERENCED_PARAMETER(_bCapture);

    if (_ulPin >= m_pMiniportPair->WaveDescriptor->PinCount)
    {
        return STATUS_INVALID_PARAMETER;
    }

    cPinFormats = GetPinSupportedDeviceFormats(_ulPin, &pPinFormats);

    for (UINT iFormat = 0; iFormat < cPinFormats; iFormat++)
    {
        PKSDATAFORMAT_WAVEFORMATEXTENSIBLE pFormat = &pPinFormats[iFormat];
        // KSDATAFORMAT validation.
        if (!IsEqualGUIDAligned(pFormat->DataFormat.MajorFormat, _pDataFormat->MajorFormat)) { continue; }
        if (!IsEqualGUIDAligned(pFormat->DataFormat.SubFormat, _pDataFormat->SubFormat)) { continue; }
        if (!IsEqualGUIDAligned(pFormat->DataFormat.Specifier, _pDataFormat->Specifier)) { continue; }
        if (pFormat->DataFormat.FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX)) { continue; }

        // WAVEFORMATEX validation.
        PWAVEFORMATEX pWaveFormat = reinterpret_cast<PWAVEFORMATEX>(_pDataFormat + 1);

        if (pWaveFormat->wFormatTag != WAVE_FORMAT_EXTENSIBLE)
        {
            if (pWaveFormat->wFormatTag != EXTRACT_WAVEFORMATEX_ID(&(pFormat->WaveFormatExt.SubFormat))) { continue; }
        }
        if (pWaveFormat->nChannels  != pFormat->WaveFormatExt.Format.nChannels) { continue; }
        if (pWaveFormat->nSamplesPerSec != pFormat->WaveFormatExt.Format.nSamplesPerSec) { continue; }
        if (pWaveFormat->nBlockAlign != pFormat->WaveFormatExt.Format.nBlockAlign) { continue; }
        if (pWaveFormat->wBitsPerSample != pFormat->WaveFormatExt.Format.wBitsPerSample) { continue; }
        if (pWaveFormat->wFormatTag != WAVE_FORMAT_EXTENSIBLE)
        {
            ntStatus = STATUS_SUCCESS;
            break;
        }

        // WAVEFORMATEXTENSIBLE validation.
        if (pWaveFormat->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) { continue; }

        PWAVEFORMATEXTENSIBLE pWaveFormatExt = reinterpret_cast<PWAVEFORMATEXTENSIBLE>(pWaveFormat);
        if (pWaveFormatExt->Samples.wValidBitsPerSample != pFormat->WaveFormatExt.Samples.wValidBitsPerSample) { continue; }
        if (pWaveFormatExt->dwChannelMask != pFormat->WaveFormatExt.dwChannelMask) { continue; }
        if (!IsEqualGUIDAligned(pWaveFormatExt->SubFormat, pFormat->WaveFormatExt.SubFormat)) { continue; }

        ntStatus = STATUS_SUCCESS;
        break;
    }

    return ntStatus;
}

#pragma code_seg("PAGE")
NTSTATUS
CMiniportWaveRT::PropertyHandlerProposedFormat
(
    _In_ PPCPROPERTY_REQUEST      PropertyRequest
)
{
    PKSP_PIN                kspPin                  = NULL;
    PKSDATAFORMAT           pKsFormat               = NULL;
    ULONG                   cbMinSize               = 0;
    NTSTATUS                ntStatus                = STATUS_INVALID_PARAMETER;

    PAGED_CODE();

    DPF_ENTER(("[CMiniportWaveRT::PropertyHandlerProposedFormat]"));

    // Every property this handler serves requires at least a KSP_PIN descriptor.
    if (PropertyRequest->InstanceSize < (sizeof(KSP_PIN) - RTL_SIZEOF_THROUGH_FIELD(KSP_PIN, Property)))
    {
        return STATUS_INVALID_PARAMETER;
    }

    kspPin = CONTAINING_RECORD(PropertyRequest->Instance, KSP_PIN, PinId);

    // Valid only on streaming pins.
    if (IsSystemRenderPin(kspPin->PinId) ||
        IsSystemCapturePin(kspPin->PinId))
    {
        ntStatus = STATUS_SUCCESS;
    }
    else if (IsBridgePin(kspPin->PinId))
    {
        ntStatus = STATUS_NOT_SUPPORTED;
    }
    else
    {
        ntStatus = STATUS_INVALID_PARAMETER;
    }

    if (!NT_SUCCESS(ntStatus))
    {
        return ntStatus;
    }

    cbMinSize = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
    {
        ULONG flags = PropertyRequest->PropertyItem->Flags;

        return PropertyHandler_BasicSupport(PropertyRequest, flags, VT_ILLEGAL);
    }

    if (PropertyRequest->ValueSize == 0)
    {
        PropertyRequest->ValueSize = cbMinSize;
        return STATUS_BUFFER_OVERFLOW;
    }
    if (PropertyRequest->ValueSize < cbMinSize)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_GET)
    {
        ntStatus = STATUS_INVALID_DEVICE_REQUEST;
    }
    else if (PropertyRequest->Verb & KSPROPERTY_TYPE_SET)
    {
        pKsFormat = (PKSDATAFORMAT)PropertyRequest->Value;
        ntStatus = IsFormatSupported(kspPin->PinId, IsSystemCapturePin(kspPin->PinId) || FALSE, pKsFormat);
        if (!NT_SUCCESS(ntStatus))
        {
            return ntStatus;
        }
    }

    return ntStatus;
} // PropertyHandlerProposedFormat

#pragma code_seg()
NTSTATUS
CMiniportWaveRT_EventHandler_PinCapsChange
(
_In_  PPCEVENT_REQUEST EventRequest
)
{
    CMiniportWaveRT* miniport = reinterpret_cast<CMiniportWaveRT*>(EventRequest->MajorTarget);
    return miniport->EventHandler_PinCapsChange(EventRequest);
}

#pragma code_seg()
NTSTATUS
CMiniportWaveRT::EventHandler_PinCapsChange
(
_In_  PPCEVENT_REQUEST EventRequest
)
{
    if (*EventRequest->EventItem->Set != KSEVENTSETID_PinCapsChange)
    {
        return STATUS_INVALID_PARAMETER;
    }

    switch (EventRequest->Verb)
    {
    case PCEVENT_VERB_SUPPORT:
        break;

    case PCEVENT_VERB_ADD:
        if (EventRequest->EventEntry)
        {
            switch (EventRequest->EventItem->Id)
            {
                // Supports the force-sample-rate feature.
                case KSEVENT_PINCAPS_FORMATCHANGE:
                    m_pPortEvents->AddEventToEventList(EventRequest->EventEntry);
                    break;
                default:
                    return STATUS_INVALID_PARAMETER;
                    break;
            }
        }
        else
        {
            return STATUS_UNSUCCESSFUL;
        }
        break;

    case PCEVENT_VERB_REMOVE:
        // The event itself can't be removed, but nothing is lost by leaving
        // it wired up and simply not acting on it further.
        break;

    default:
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
NTSTATUS
CMiniportWaveRT::PropertyHandlerProposedFormat2
(
    _In_ PPCPROPERTY_REQUEST      PropertyRequest
)
{
    PKSP_PIN                kspPin                  = NULL;
    ULONG                   cbMinSize               = 0;
    NTSTATUS                ntStatus                = STATUS_INVALID_PARAMETER;
    ULONG                   numModes                = 0;
    MODE_AND_DEFAULT_FORMAT *modeInfo               = NULL;
    MODE_AND_DEFAULT_FORMAT *modeTemp               = NULL;
    PKSMULTIPLE_ITEM        pKsItemsHeader          = NULL;
    PKSMULTIPLE_ITEM        pKsItemsHeaderOut       = NULL;
    size_t                  cbItemsList             = 0;
    GUID                    signalProcessingMode    = {0};
    BOOLEAN                 bFound                  = FALSE;
    ULONG                   i;

    PAGED_CODE();

    DPF_ENTER(("[CMiniportWaveRT::PropertyHandlerProposedFormat2]"));

    // Every property this handler serves requires at least a KSP_PIN descriptor.
    if (PropertyRequest->InstanceSize < (sizeof(KSP_PIN) - RTL_SIZEOF_THROUGH_FIELD(KSP_PIN, Property)))
    {
        return STATUS_INVALID_PARAMETER;
    }

    kspPin = CONTAINING_RECORD(PropertyRequest->Instance, KSP_PIN, PinId);

    if (kspPin->PinId >= m_pMiniportPair->WaveDescriptor->PinCount)
    {
        return STATUS_INVALID_PARAMETER;
    }

    // Supported only on some streaming pins.
    numModes = GetPinSupportedDeviceModes(kspPin->PinId, &modeInfo);

    ASSERT((modeInfo != NULL && numModes > 0) || (modeInfo == NULL && numModes == 0));

    if (modeInfo == NULL)
    {
        return STATUS_NOT_SUPPORTED;
    }

    // Even for pins that support modes, the pin might not support proposed formats.
    bFound = FALSE;
    for (i=0, modeTemp=modeInfo; i<numModes; ++i, ++modeTemp)
    {
        if (modeTemp->DefaultFormat != NULL)
        {
            bFound = TRUE;
            break;
        }
    }

    if (!bFound)
    {
        return STATUS_NOT_SUPPORTED;
    }

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
    {
        return PropertyHandler_BasicSupport(PropertyRequest, PropertyRequest->PropertyItem->Flags, VT_ILLEGAL);
    }

    pKsItemsHeader = (PKSMULTIPLE_ITEM)(kspPin + 1);
    cbItemsList = (((PBYTE)PropertyRequest->Instance) + PropertyRequest->InstanceSize) - (PBYTE)pKsItemsHeader;

    ntStatus = GetAttributesFromAttributeList(pKsItemsHeader, cbItemsList, &signalProcessingMode);
    if (!NT_SUCCESS(ntStatus))
    {
        return ntStatus;
    }

    bFound = FALSE;
    for (i=0; i<numModes; ++i, ++modeInfo)
    {
        if (modeInfo->Mode == signalProcessingMode)
        {
            bFound = TRUE;
            break;
        }
    }

    // Either the mode isn't supported, or the driver doesn't have a
    // proposed format for this specific mode.
    if (!bFound || modeInfo->DefaultFormat == NULL)
    {
        return STATUS_NOT_SUPPORTED;
    }

    cbMinSize = modeInfo->DefaultFormat->FormatSize;
    cbMinSize = (cbMinSize + 7) & ~7;

    pKsItemsHeaderOut = (PKSMULTIPLE_ITEM)((PBYTE)PropertyRequest->Value + cbMinSize);

    if (cbItemsList > MAXULONG)
    {
        return STATUS_INVALID_PARAMETER;
    }

    ntStatus = RtlULongAdd(cbMinSize, (ULONG)cbItemsList, &cbMinSize);
    if (!NT_SUCCESS(ntStatus))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (cbMinSize == 0)
    {
        return STATUS_NOT_SUPPORTED;
    }

    if (PropertyRequest->ValueSize == 0)
    {
        PropertyRequest->ValueSize = cbMinSize;
        return STATUS_BUFFER_OVERFLOW;
    }
    if (PropertyRequest->ValueSize < cbMinSize)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    // Only GET is supported for this property.
    if ((PropertyRequest->Verb & KSPROPERTY_TYPE_GET) == 0)
    {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    RtlCopyMemory(PropertyRequest->Value, modeInfo->DefaultFormat, modeInfo->DefaultFormat->FormatSize);

    ASSERT(cbItemsList > 0);
    ((KSDATAFORMAT*)PropertyRequest->Value)->Flags = KSDATAFORMAT_ATTRIBUTES;
    RtlCopyMemory(pKsItemsHeaderOut, pKsItemsHeader, cbItemsList);

    PropertyRequest->ValueSize = cbMinSize;

    return STATUS_SUCCESS;
} // PropertyHandlerProposedFormat

// Mixes DRM rights across every active stream: gathers each stream's content
// ID, asks DrmPort to mint a combined content ID with a mixed DRMRIGHTS, and
// (in a real driver) that result should gate whether streams may be written
// to disk/copied. This driver only enforces it via CopyProtect on the
// file-saving path.
#pragma code_seg("PAGE")
NTSTATUS
CMiniportWaveRT::UpdateDrmRights
(
    void
)
{
    PAGED_CODE();

    DPF_ENTER(("[CMiniportWaveRT::UpdateDrmRights]"));

    NTSTATUS        ntStatus                = STATUS_UNSUCCESSFUL;
    ULONG           ulMixDrmContentId       = 0;
    BOOL            fCreatedContentId       = FALSE;
    DRMRIGHTS       MixDrmRights            = {FALSE, 0, FALSE};
    ULONG           ulContentIndex          = 0;
    ULONG*          ulContentIds            = NULL;

    // Only runs if IID_DrmPort is implemented in the wave port.
    if (!m_pDrmPort)
    {
        return STATUS_UNSUCCESSFUL;
    }

    ulContentIds = new (POOL_FLAG_NON_PAGED, MINWAVERT_POOLTAG) ULONG[m_ulMaxSystemStreams + m_ulMaxOffloadStreams];
    if (!ulContentIds)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (ULONG i = 0; i < m_ulMaxSystemStreams; i++)
    {
        if (m_SystemStreams[i])
        {
            ulContentIds[ulContentIndex] = m_SystemStreams[i]->m_ulContentId;
            ulContentIndex++;
        }
    }

    if (ulContentIndex)
    {
        ntStatus =
            m_pDrmPort->CreateContentMixed
            (
                ulContentIds,
                ulContentIndex,
                &ulMixDrmContentId
            );

        if (NT_SUCCESS(ntStatus))
        {
            fCreatedContentId = TRUE;
            ntStatus =
                m_pDrmPort->GetContentRights
                (
                    ulMixDrmContentId,
                    &MixDrmRights
                );
        }
    }

    if (NT_SUCCESS(ntStatus))
    {
        m_pDrmPort->DestroyContent(m_ulMixDrmContentId);
        m_ulMixDrmContentId = ulMixDrmContentId;
        RtlCopyMemory(&m_MixDrmRights, &MixDrmRights, sizeof(m_MixDrmRights));
    }

    if (!NT_SUCCESS(ntStatus) && fCreatedContentId)
    {
        m_pDrmPort->DestroyContent(ulMixDrmContentId);
    }

    ASSERT(ulContentIds);
    delete [] ulContentIds;
    ulContentIds = NULL;

    return ntStatus;
} // UpdateDrmRights

// PortCls entry point for wave filter property requests; MajorTarget is the miniport instance.
#pragma code_seg("PAGE")
NTSTATUS
PropertyHandler_WaveFilter
(
    _In_ PPCPROPERTY_REQUEST      PropertyRequest
)
{
    PAGED_CODE();

    NTSTATUS            ntStatus = STATUS_INVALID_DEVICE_REQUEST;
    CMiniportWaveRT*    pWaveHelper = reinterpret_cast<CMiniportWaveRT*>(PropertyRequest->MajorTarget);

    if (pWaveHelper == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    pWaveHelper->AddRef();

    if (IsEqualGUIDAligned(*PropertyRequest->PropertyItem->Set, KSPROPSETID_Pin))
    {
        switch (PropertyRequest->PropertyItem->Id)
        {
            case KSPROPERTY_PIN_PROPOSEDATAFORMAT:
                ntStatus = pWaveHelper->PropertyHandlerProposedFormat(PropertyRequest);
                break;

            case KSPROPERTY_PIN_PROPOSEDATAFORMAT2:
                ntStatus = pWaveHelper->PropertyHandlerProposedFormat2(PropertyRequest);
                break;

            default:
                DPF(D_TERSE, ("[PropertyHandler_WaveFilter: Invalid Device Request]"));
        }
    }

    pWaveHelper->Release();

    return ntStatus;
} // PropertyHandler_WaveFilter

#pragma code_seg("PAGE")
NTSTATUS
PropertyHandler_GenericPin
(
    _In_ PPCPROPERTY_REQUEST      PropertyRequest
)
{
    NTSTATUS                ntStatus = STATUS_INVALID_DEVICE_REQUEST;
    CMiniportWaveRT*        pWave = NULL;
    CMiniportWaveRTStream * pStream = NULL;

    PAGED_CODE();

    if (PropertyRequest->MajorTarget == NULL ||
        PropertyRequest->MinorTarget == NULL)
    {
        ntStatus = STATUS_INVALID_PARAMETER;
        goto exit;
    }

    pWave = MajorTarget_to_Obj(PropertyRequest->MajorTarget);
    pWave->AddRef();

    pStream = MinorTarget_to_Obj(PropertyRequest->MinorTarget);
    pStream->AddRef();

exit:

    SAFE_RELEASE(pStream);
    SAFE_RELEASE(pWave);

    return ntStatus;
}

#pragma code_seg()
