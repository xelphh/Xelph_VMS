/*++

Copyright (c) Xelph. All Rights Reserved.

Module Name:

    streamworker.cpp

Abstract:

    Implementation of CMiniportWaveRTStream, the per-stream worker created
    for each open audio stream (see streamworker.h). Owns DMA buffer
    allocation, the WaveRT position/packet/notification-timer machinery
    (GetPosition, GetReadPacket/SetWritePacket, TimerNotifyRT), and the
    per-sample-tick data path: WriteBytes fills a capture stream from the
    speaker->microphone loopback ring buffer (falling back to silence if
    loopback is disabled or nothing has ever played), WriteToLoopback pushes
    newly-rendered speaker audio into that same ring buffer, and ReadBytes
    optionally saves rendered audio to disk for debugging.

Contact:

    Discord: xelphh
    Website: https://Xelph.lol

--*/

#include "shared.h"
#include <limits.h>
#include <ks.h>
#include "pinmap.h"
#include "waveengine.h"
#include "streamworker.h"
#include "looprelay.h"
#define MINWAVERTSTREAM_POOLTAG 'SRWM'
// # of frames converted per pass when pushing to/pulling from the loopback
// ring buffer; keeps the on-stack conversion buffers small since this runs
// at DISPATCH_LEVEL.
#define LOOPBACK_CHUNK_FRAMES 64

#pragma warning (disable : 4127)

#pragma code_seg("PAGE")
CMiniportWaveRTStream::~CMiniportWaveRTStream
(
    void
)
{
    PAGED_CODE();
    if (NULL != m_pMiniport)
    {

        if (m_bUnregisterStream)
        {
            m_pMiniport->StreamClosed(m_ulPin, this);
            m_bUnregisterStream = FALSE;
        }

        m_pMiniport->Release();
        m_pMiniport = NULL;
    }

    if (m_pDpc)
    {
        ExFreePoolWithTag( m_pDpc, MINWAVERTSTREAM_POOLTAG );
        m_pDpc = NULL;
    }

    if (m_pTimer)
    {
        ExFreePoolWithTag( m_pTimer, MINWAVERTSTREAM_POOLTAG );
        m_pTimer = NULL;
    }

    if (m_pbMuted)
    {
        ExFreePoolWithTag( m_pbMuted, MINWAVERTSTREAM_POOLTAG );
        m_pbMuted = NULL;
    }

    if (m_plVolumeLevel)
    {
        ExFreePoolWithTag( m_plVolumeLevel, MINWAVERTSTREAM_POOLTAG );
        m_plVolumeLevel = NULL;
    }

    if (m_plPeakMeter)
    {
        ExFreePoolWithTag( m_plPeakMeter, MINWAVERTSTREAM_POOLTAG );
        m_plPeakMeter = NULL;
    }

    if (m_pWfExt)
    {
        ExFreePoolWithTag( m_pWfExt, MINWAVERTSTREAM_POOLTAG );
        m_pWfExt = NULL;
    }
    if (m_pNotificationTimer)
    {
        ExDeleteTimer
        (
            m_pNotificationTimer,
            TRUE, // Cancel the timer if it is currently set.
            TRUE, // Wait for the timer to finish expiring and for any callback to a ExTimerCallback routine to finish.
            NULL
         );
    }

    // Since we just cancelled the notification timer, wait for all queued
    // DPCs to complete before we free the notification DPC.
    KeFlushQueuedDpcs();

    DPF_ENTER(("[CMiniportWaveRTStream::~CMiniportWaveRTStream]"));
} // ~CMiniportWaveRTStream

#pragma code_seg("PAGE")

NTSTATUS CMiniportWaveRTStream::ReadRegistrySettings()
{
    PAGED_CODE();

    NTSTATUS                    ntStatus;
    PDRIVER_OBJECT              DriverObject;
    HANDLE                      DriverKey;
    RTL_QUERY_REGISTRY_TABLE    paramTable[] = {
        // QueryRoutine     Flags                                               Name                            EntryContext                            DefaultType                                                     DefaultData                                 DefaultLength
        { NULL,   RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK, L"HostCaptureToneFrequency",        &m_ulHostCaptureToneFrequency,          (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD,  &m_ulHostCaptureToneFrequency,              sizeof(DWORD) },
        { NULL,   RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK, L"HostCaptureToneAmplitude",        &m_dwHostCaptureToneAmplitude,          (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD,  &m_dwHostCaptureToneAmplitude,              sizeof(DWORD) },
        { NULL,   RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK, L"HostCaptureToneDCOffset",         &m_dwHostCaptureToneDCOffset,           (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD,  &m_dwHostCaptureToneDCOffset,               sizeof(DWORD) },
        { NULL,   RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK, L"HostCaptureToneInitialPhase",     &m_dwHostCaptureToneInitialPhase,       (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD,  &m_dwHostCaptureToneInitialPhase,           sizeof(DWORD) },
        { NULL,   0,                                                        NULL,                               NULL,                                   0,                                                              NULL,                                       0 }
    };

    DriverObject = WdfDriverWdmGetDriverObject(WdfGetDriver());
    DriverKey = NULL;
    ntStatus = IoOpenDriverRegistryKey(DriverObject,
                                 DriverRegKeyParameters,
                                 KEY_READ,
                                 0,
                                 &DriverKey);

    if (!NT_SUCCESS(ntStatus))
    {
        return ntStatus;
    }

    ntStatus = RtlQueryRegistryValues(RTL_REGISTRY_HANDLE,
                                  (PCWSTR) DriverKey,
                                  &paramTable[0],
                                  NULL,
                                  NULL);

    if (!NT_SUCCESS(ntStatus))
    {
        // Don't return an error - we just operate with the defaults set above.
        DPF(D_VERBOSE, ("RtlQueryRegistryValues failed, using default values, 0x%x", ntStatus));
    }

    if (DriverKey)
    {
        ZwClose(DriverKey);
    }

    return ntStatus;
}

NTSTATUS
CMiniportWaveRTStream::Init
(
    _In_ PCMiniportWaveRT           Miniport_,
    _In_ PPORTWAVERTSTREAM          PortStream_,
    _In_ ULONG                      Pin_,
    _In_ BOOLEAN                    Capture_,
    _In_ PKSDATAFORMAT              DataFormat_,
    _In_ GUID                       SignalProcessingMode
)
{
    PAGED_CODE();

    PWAVEFORMATEX pWfEx = NULL;
    NTSTATUS ntStatus = STATUS_SUCCESS;

    m_pMiniport = NULL;
    m_ulPin = 0;
    m_bUnregisterStream = FALSE;
    m_bCapture = FALSE;
    m_ulDmaBufferSize = 0;
    m_pDmaBuffer = NULL;
    m_ulNotificationsPerBuffer = 0;
    m_KsState = KSSTATE_STOP;
    m_pTimer = NULL;
    m_pDpc = NULL;
    m_llPacketCounter = 0;
    m_ullPlayPosition = 0;
    m_ullWritePosition = 0;
    m_ullDmaTimeStamp = 0;
    m_hnsElapsedTimeCarryForward = 0;
    m_ullLastDPCTimeStamp = 0;
    m_hnsDPCTimeCarryForward = 0;
    m_ulDmaMovementRate = 0;
    m_byteDisplacementCarryForward = 0;
    m_bLfxEnabled = FALSE;
    m_pbMuted = NULL;
    m_plVolumeLevel = NULL;
    m_plPeakMeter = NULL;
    m_pWfExt = NULL;
    m_ullLinearPosition = 0;
    m_ullPresentationPosition = 0;
    m_ulContentId = 0;
    m_ullLoopbackFramesProcessed = 0;
    m_ulCurrentWritePosition = 0;
    m_ulLastOsReadPacket = ULONG_MAX;
    m_ulLastOsWritePacket = ULONG_MAX;
    m_IsCurrentWritePositionUpdated = 0;
    m_SignalProcessingMode = SignalProcessingMode;
    m_bEoSReceived = FALSE;
    m_bLastBufferRendered = FALSE;

    m_ulHostCaptureToneFrequency = IsEqualGUID(SignalProcessingMode, AUDIO_SIGNALPROCESSINGMODE_RAW) ? 1000 : 2000;
    m_dwHostCaptureToneAmplitude = 50;
    m_dwHostCaptureToneDCOffset = 0;
    m_dwHostCaptureToneInitialPhase = 0;

    m_pPortStream = PortStream_;
    InitializeListHead(&m_NotificationList);
    m_ulNotificationIntervalMs = 0;

    KeInitializeSpinLock(&m_PositionSpinLock);

    m_pNotificationTimer = ExAllocateTimer(
         TimerNotifyRT,
         this,
         EX_TIMER_HIGH_RESOLUTION
    );
    if (!m_pNotificationTimer)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    pWfEx = GetWaveFormatEx(DataFormat_);
    if (NULL == pWfEx)
    {
        return STATUS_UNSUCCESSFUL;
    }

    m_pMiniport = reinterpret_cast<CMiniportWaveRT*>(Miniport_);
    if (m_pMiniport == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    m_pMiniport->AddRef();
    if (!NT_SUCCESS(ntStatus))
    {
        return ntStatus;
    }
    m_ulPin = Pin_;
    m_bCapture = Capture_;
    m_ulDmaMovementRate = pWfEx->nAvgBytesPerSec;

    m_pDpc = (PRKDPC)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(KDPC), MINWAVERTSTREAM_POOLTAG);
    if (!m_pDpc)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    m_pWfExt = (PWAVEFORMATEXTENSIBLE)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(WAVEFORMATEX) + pWfEx->cbSize, MINWAVERTSTREAM_POOLTAG);
    if (m_pWfExt == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(m_pWfExt, pWfEx, sizeof(WAVEFORMATEX) + pWfEx->cbSize);

    m_pbMuted = (PBOOL)ExAllocatePool2(POOL_FLAG_NON_PAGED, m_pWfExt->Format.nChannels * sizeof(BOOL), MINWAVERTSTREAM_POOLTAG);
    if (m_pbMuted == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    m_plVolumeLevel = (PLONG)ExAllocatePool2(POOL_FLAG_NON_PAGED, m_pWfExt->Format.nChannels * sizeof(LONG), MINWAVERTSTREAM_POOLTAG);
    if (m_plVolumeLevel == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    m_plPeakMeter = (PLONG)ExAllocatePool2(POOL_FLAG_NON_PAGED, m_pWfExt->Format.nChannels * sizeof(LONG), MINWAVERTSTREAM_POOLTAG);
    if (m_plPeakMeter == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (m_bCapture)
    {
        ReadRegistrySettings();
        DWORD toneFrequency = 0;
        DWORD toneAmplitude = 0;
        DWORD toneDCOffset = 0;
        DWORD toneInitialPhase = 0;

        double toneAmplitudeDouble = 0;
        double toneDCOffsetDouble = 0;
        double toneInitialPhaseDouble = 0;

        toneFrequency = m_ulHostCaptureToneFrequency;
        toneAmplitude = m_dwHostCaptureToneAmplitude;
        toneDCOffset = m_dwHostCaptureToneDCOffset;
        toneInitialPhase = m_dwHostCaptureToneInitialPhase;

        if (labs(toneAmplitude) > 100)
        {
            toneAmplitude = toneAmplitude > 0 ? 100 : -100;
        }

        if (labs(toneDCOffset) > 100)
        {
            toneDCOffset = toneDCOffset > 0 ? 100 : -100;
        }

        DWORD abssum = labs(toneAmplitude) + labs(toneDCOffset);

        if (abssum > 100)
        {
            toneAmplitudeDouble = ((double)toneAmplitude) / abssum;
            toneDCOffsetDouble = ((double)toneDCOffset) / abssum;
        }
        else
        {
            toneAmplitudeDouble = ((double)toneAmplitude) / 100.0;
            toneDCOffsetDouble = ((double)toneDCOffset) / 100.0;
        }

        if (labs(toneInitialPhase) > 31416)
        {
            toneInitialPhase = toneInitialPhase > 0 ? 31416 : -31416;
        }

        toneInitialPhaseDouble = (double)toneInitialPhase / 10000;

        ntStatus = m_ToneGenerator.Init(toneFrequency, toneAmplitudeDouble, toneDCOffsetDouble, toneInitialPhaseDouble, m_pWfExt);

        if (!NT_SUCCESS(ntStatus))
        {
            return ntStatus;
        }
    }
    else if (!g_DoNotCreateDataFiles)
    {
        DPF(D_TERSE, ("SaveData %p", &m_SaveData));
        ntStatus = m_SaveData.SetDataFormat(DataFormat_);
        if (NT_SUCCESS(ntStatus))
        {
            ntStatus = m_SaveData.Initialize();
        }

        if (!NT_SUCCESS(ntStatus))
        {
            return ntStatus;
        }
    }

    ntStatus = m_pMiniport->StreamCreated(m_ulPin, this);
    if (NT_SUCCESS(ntStatus))
    {
        m_bUnregisterStream = TRUE;
    }

    return ntStatus;
} // Init

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::NonDelegatingQueryInterface
(
    _In_ REFIID  Interface,
    _COM_Outptr_ PVOID * Object
)
{
    PAGED_CODE();

    ASSERT(Object);

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = PVOID(PUNKNOWN(PMINIPORTWAVERTSTREAM(this)));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTStream))
    {
        *Object = PVOID(PMINIPORTWAVERTSTREAM(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTStreamNotification))
    {
        *Object = PVOID(PMINIPORTWAVERTSTREAMNOTIFICATION(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTInputStream) && (this->m_bCapture))
    {
        // Supported only on capture streams.
        *Object = PVOID(PMINIPORTWAVERTINPUTSTREAM(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTOutputStream))
    {
        // Supported only on host render streams.
        *Object = PVOID(PMINIPORTWAVERTOUTPUTSTREAM(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IDrmAudioStream))
    {
        *Object = (PVOID)(IDrmAudioStream*)this;
    }
    else
    {
        *Object = NULL;
    }

    if (*Object)
    {
        PUNKNOWN(*Object)->AddRef();
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_PARAMETER;
} // NonDelegatingQueryInterface

#pragma code_seg("PAGE")
NTSTATUS CMiniportWaveRTStream::AllocateBufferWithNotification
(
    _In_    ULONG               NotificationCount_,
    _In_    ULONG               RequestedSize_,
    _Out_   PMDL                *AudioBufferMdl_,
    _Out_   ULONG               *ActualSize_,
    _Out_   ULONG               *OffsetFromFirstPage_,
    _Out_   MEMORY_CACHING_TYPE *CacheType_
)
{
    PAGED_CODE();

    ULONG ulBufferDurationMs = 0;

    if ( (0 == RequestedSize_) || (RequestedSize_ < m_pWfExt->Format.nBlockAlign) )
    {
        return STATUS_UNSUCCESSFUL;
    }

    if ((NotificationCount_ == 0) || (RequestedSize_ % NotificationCount_ != 0))
    {
        return STATUS_INVALID_PARAMETER;
    }

    RequestedSize_ -= RequestedSize_ % (m_pWfExt->Format.nBlockAlign);

    if (!m_bCapture && (!g_DoNotCreateDataFiles))
    {
        NTSTATUS ntStatus;

        // A larger buffer here means fewer file I/O operations when saving to disk.
        ntStatus = m_SaveData.SetMaxWriteSize(RequestedSize_ * 4);
        if (!NT_SUCCESS(ntStatus))
        {
            return ntStatus;
        }
    }

    PHYSICAL_ADDRESS highAddress;
    highAddress.HighPart = 0;
    highAddress.LowPart = MAXULONG;

    PMDL pBufferMdl = m_pPortStream->AllocatePagesForMdl (highAddress, RequestedSize_);

    if (NULL == pBufferMdl)
    {
        return STATUS_UNSUCCESSFUL;
    }

    // The Windows audio stack has no way to express memory-alignment
    // requirements for buffers, so MmCached is used here deliberately -
    // it's the caching type least likely to impose platform-specific
    // alignment assumptions on the mapped buffer.
    m_pDmaBuffer = (BYTE*)m_pPortStream->MapAllocatedPages(pBufferMdl, MmCached);
    m_ulNotificationsPerBuffer = NotificationCount_;
    m_ulDmaBufferSize = RequestedSize_;
    ulBufferDurationMs = (RequestedSize_ * 1000) / m_ulDmaMovementRate;
    m_ulNotificationIntervalMs = ulBufferDurationMs / NotificationCount_;

    *AudioBufferMdl_ = pBufferMdl;
    *ActualSize_ = RequestedSize_;
    *OffsetFromFirstPage_ = 0;
    *CacheType_ = MmCached;

    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
VOID CMiniportWaveRTStream::FreeBufferWithNotification
(
    _In_        PMDL    Mdl_,
    _In_        ULONG   Size_
)
{
    UNREFERENCED_PARAMETER(Size_);

    PAGED_CODE();

    if (Mdl_ != NULL)
    {
        if (m_pDmaBuffer != NULL)
        {
            m_pPortStream->UnmapAllocatedPages(m_pDmaBuffer, Mdl_);
            m_pDmaBuffer = NULL;
        }

        m_pPortStream->FreePagesFromMdl(Mdl_);
    }

    m_ulDmaBufferSize = 0;
    m_ulNotificationsPerBuffer = 0;

    return;
}

#pragma code_seg("PAGE")
NTSTATUS CMiniportWaveRTStream::RegisterNotificationEvent
(
    _In_ PKEVENT NotificationEvent_
)
{
    UNREFERENCED_PARAMETER(NotificationEvent_);

    PAGED_CODE();

    NotificationListEntry *nleNew = (NotificationListEntry*)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(NotificationListEntry),
        MINWAVERTSTREAM_POOLTAG);
    if (NULL == nleNew)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    nleNew->NotificationEvent = NotificationEvent_;

    // Fail if the notification event already exists in our list.
    if (!IsListEmpty(&m_NotificationList))
    {
        PLIST_ENTRY leCurrent = m_NotificationList.Flink;
        while (leCurrent != &m_NotificationList)
        {
            NotificationListEntry* nleCurrent = CONTAINING_RECORD( leCurrent, NotificationListEntry, ListEntry);
            if (nleCurrent->NotificationEvent == NotificationEvent_)
            {
                ExFreePoolWithTag( nleNew, MINWAVERTSTREAM_POOLTAG );
                return STATUS_UNSUCCESSFUL;
            }

            leCurrent = leCurrent->Flink;
        }
    }

    InsertTailList(&m_NotificationList, &(nleNew->ListEntry));

    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
NTSTATUS CMiniportWaveRTStream::UnregisterNotificationEvent
(
    _In_ PKEVENT NotificationEvent_
)
{
    UNREFERENCED_PARAMETER(NotificationEvent_);

    PAGED_CODE();

    if (!IsListEmpty(&m_NotificationList))
    {
        PLIST_ENTRY leCurrent = m_NotificationList.Flink;
        while (leCurrent != &m_NotificationList)
        {
            NotificationListEntry* nleCurrent = CONTAINING_RECORD( leCurrent, NotificationListEntry, ListEntry);
            if (nleCurrent->NotificationEvent == NotificationEvent_)
            {
                RemoveEntryList( leCurrent );
                ExFreePoolWithTag( nleCurrent, MINWAVERTSTREAM_POOLTAG );
                return STATUS_SUCCESS;
            }

            leCurrent = leCurrent->Flink;
        }
    }

    return STATUS_NOT_FOUND;
}

#pragma code_seg("PAGE")
NTSTATUS CMiniportWaveRTStream::GetClockRegister
(
    _Out_ PKSRTAUDIO_HWREGISTER Register_
)
{
    UNREFERENCED_PARAMETER(Register_);

    PAGED_CODE();

    return STATUS_NOT_IMPLEMENTED;
}

#pragma code_seg("PAGE")
NTSTATUS CMiniportWaveRTStream::GetPositionRegister
(
    _Out_ PKSRTAUDIO_HWREGISTER Register_
)
{
    UNREFERENCED_PARAMETER(Register_);

    PAGED_CODE();

    return STATUS_NOT_IMPLEMENTED;
}

#pragma code_seg("PAGE")
VOID CMiniportWaveRTStream::GetHWLatency
(
    _Out_ PKSRTAUDIO_HWLATENCY  Latency_
)
{
    PAGED_CODE();

    ASSERT(Latency_);

    Latency_->ChipsetDelay = 0;
    Latency_->CodecDelay = 0;
    Latency_->FifoSize = 0;
}

#pragma code_seg("PAGE")
VOID CMiniportWaveRTStream::FreeAudioBuffer
(
_In_opt_    PMDL        Mdl_,
_In_        ULONG       Size_
)
{
    UNREFERENCED_PARAMETER(Size_);

    PAGED_CODE();

    if (Mdl_ != NULL)
    {
        if (m_pDmaBuffer != NULL)
        {
            m_pPortStream->UnmapAllocatedPages(m_pDmaBuffer, Mdl_);
            m_pDmaBuffer = NULL;
        }

        m_pPortStream->FreePagesFromMdl(Mdl_);
    }

    m_ulDmaBufferSize = 0;
    m_ulNotificationsPerBuffer = 0;
}

#pragma code_seg("PAGE")
NTSTATUS CMiniportWaveRTStream::AllocateAudioBuffer
(
_In_    ULONG                   RequestedSize_,
_Out_   PMDL                   *AudioBufferMdl_,
_Out_   ULONG                  *ActualSize_,
_Out_   ULONG                  *OffsetFromFirstPage_,
_Out_   MEMORY_CACHING_TYPE    *CacheType_
)
{
    PAGED_CODE();

    if ((0 == RequestedSize_) || (RequestedSize_ < m_pWfExt->Format.nBlockAlign))
    {
        return STATUS_UNSUCCESSFUL;
    }

    RequestedSize_ -= RequestedSize_ % (m_pWfExt->Format.nBlockAlign);

    PHYSICAL_ADDRESS highAddress;
    highAddress.HighPart = 0;
    highAddress.LowPart = MAXULONG;

    PMDL pBufferMdl = m_pPortStream->AllocatePagesForMdl(highAddress, RequestedSize_);

    if (NULL == pBufferMdl)
    {
        return STATUS_UNSUCCESSFUL;
    }

    // See the caching-type note in AllocateBufferWithNotification above.
    m_pDmaBuffer = (BYTE*)m_pPortStream->MapAllocatedPages(pBufferMdl, MmCached);

    m_ulDmaBufferSize = RequestedSize_;
    m_ulNotificationsPerBuffer = 0;

    *AudioBufferMdl_ = pBufferMdl;
    *ActualSize_ = RequestedSize_;
    *OffsetFromFirstPage_ = 0;
    *CacheType_ = MmCached;

    return STATUS_SUCCESS;
}

#pragma code_seg()
NTSTATUS CMiniportWaveRTStream::GetPosition
(
    _Out_   KSAUDIO_POSITION    *Position_
)
{
    NTSTATUS ntStatus;

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_PositionSpinLock, &oldIrql);

    if (m_KsState == KSSTATE_RUN)
    {
        LARGE_INTEGER ilQPC = KeQueryPerformanceCounter(NULL);
        UpdatePosition(ilQPC);
    }

    Position_->PlayOffset = m_ullPlayPosition;
    Position_->WriteOffset = m_ullWritePosition;

    KeReleaseSpinLock(&m_PositionSpinLock, oldIrql);

    ntStatus = STATUS_SUCCESS;

    return ntStatus;
}

// Returns info about the next packet for the OS to read, or
// STATUS_DEVICE_NOT_READY if none are available yet. Called at
// PASSIVE_LEVEL but kept in non-paged code since it's on the streaming path,
// where page faults must be avoided.
#pragma code_seg()
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS CMiniportWaveRTStream::GetReadPacket
(
    _Out_ ULONG* PacketNumber,
    _Out_ DWORD* Flags,
    _Out_ ULONG64* PerformanceCounterValue,
    _Out_ BOOL* MoreData
)
{
    ULONG availablePacketNumber;
    ULONG droppedPackets;

    // The call must be from event driven mode.
    if (m_ulNotificationsPerBuffer == 0)
    {
        return STATUS_NOT_SUPPORTED;
    }

    *Flags = 0;

    if (m_KsState < KSSTATE_PAUSE)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_PositionSpinLock, &oldIrql);

    LONGLONG packetCounter = m_llPacketCounter;
    ULONGLONG ullLinearPosition = m_ullLinearPosition;
    ULONGLONG hnsElapsedTimeCarryForward = m_hnsElapsedTimeCarryForward;
    ULONGLONG ullDmaTimeStamp = m_ullDmaTimeStamp;

    KeReleaseSpinLock(&m_PositionSpinLock, oldIrql);

    // The 0-based number of the last completed packet (may be ULONG_MAX
    // during the very first packet).
    availablePacketNumber = LODWORD(packetCounter - 1);

    if (availablePacketNumber == m_ulLastOsReadPacket)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    // More than one packet transferred since the OS last read - those were dropped (a glitch).
    droppedPackets = availablePacketNumber - m_ulLastOsReadPacket - 1;
    if (droppedPackets > 0)
    {
        // Trace a glitch
    }

    *PacketNumber = availablePacketNumber;

    // Compute the timestamp for the end of the available packet. Real hardware would
    // compute this in a driver/hardware-specific way; here it's extrapolated from the
    // simulated position correlation [m_ullLinearPosition @ m_ullDmaTimeStamp] and the
    // packet counter, subtracting 1 to get the start time of that last completed packet.
    ULONGLONG linearPositionOfAvailablePacket = packetCounter * (m_ulDmaBufferSize / m_ulNotificationsPerBuffer);
    // Divide by 1000 * 10000 because m_ulDmaMovementRate is average bytes/sec.
    ULONGLONG carryForwardBytes = (hnsElapsedTimeCarryForward * m_ulDmaMovementRate) / 10000000;
    ULONGLONG deltaLinearPosition = ullLinearPosition + carryForwardBytes - linearPositionOfAvailablePacket;
    ULONGLONG deltaTimeInHns = deltaLinearPosition * 10000000 / m_ulDmaMovementRate;
    ULONGLONG timeOfAvailablePacketInHns = ullDmaTimeStamp - deltaTimeInHns;
    ULONGLONG timeOfAvailablePacketInQpc = timeOfAvailablePacketInHns * m_ullPerformanceCounterFrequency.QuadPart / 10000000;

    *PerformanceCounterValue = timeOfAvailablePacketInQpc;

    *Flags = 0;

    // This driver doesn't buffer internally, so there's never more data than this call reveals.
    *MoreData = FALSE;

    m_ulLastOsReadPacket = availablePacketNumber;

    return STATUS_SUCCESS;
}

#pragma code_seg()
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS CMiniportWaveRTStream::SetWritePacket
(
    _In_ ULONG      PacketNumber,
    _In_ DWORD      Flags,
    _In_ ULONG      EosPacketLength
)
{
    UNREFERENCED_PARAMETER(EosPacketLength);
    NTSTATUS ntStatus;

    // The call must be from event driven mode.
    if (m_ulNotificationsPerBuffer == 0)
    {
        return STATUS_NOT_SUPPORTED;
    }

    ULONG oldLastOsWritePacket = m_ulLastOsWritePacket;

    // This function should not be called once EoS has been set.
    if (m_bEoSReceived)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_PositionSpinLock, &oldIrql);
    // 1-based count of completed packets, 0-based packet number of current packet.
    LONGLONG currentPacket = m_llPacketCounter;
    KeReleaseSpinLock(&m_PositionSpinLock, oldIrql);

    // If not running, the current packet hasn't started transferring yet so the OS
    // should write to it; if running, the current packet is already in flight so the
    // OS should write the one after it.
    ULONG expectedPacket = LODWORD(currentPacket);
    if (m_KsState == KSSTATE_RUN)
    {
        expectedPacket++;
    }

    // Modulo arithmetic: negative means behind, positive means too far ahead.
    LONG deltaFromExpectedPacket = PacketNumber - expectedPacket;
    if (deltaFromExpectedPacket < 0)
    {
        return STATUS_DATA_LATE_ERROR;
    }
    else if (deltaFromExpectedPacket > 0)
    {
        return STATUS_DATA_OVERRUN;
    }

    ULONG packetSize = (m_ulDmaBufferSize / m_ulNotificationsPerBuffer);
    ULONG packetIndex = PacketNumber % m_ulNotificationsPerBuffer;
    ULONG ulCurrentWritePosition = packetIndex * packetSize;

    if (Flags & KSSTREAM_HEADER_OPTIONSF_ENDOFSTREAM)
    {
        return STATUS_INVALID_PARAMETER;
    }
    else
    {
        m_ulLastOsWritePacket = PacketNumber;

        // Sets the current write position to the given byte in the DMA buffer, after
        // validating it against the buffer size and de-duping repeated calls with the
        // same position (in event mode); underrun is checked separately via the timer.
        KeAcquireSpinLock(&m_PositionSpinLock, &oldIrql);
        ntStatus = SetCurrentWritePositionInternal(ulCurrentWritePosition);
        KeReleaseSpinLock(&m_PositionSpinLock, oldIrql);
    }

    if (!NT_SUCCESS(ntStatus))
    {
        m_ulLastOsWritePacket = oldLastOsWritePacket;
    }

    return ntStatus;
}

#pragma code_seg()
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS CMiniportWaveRTStream::GetOutputStreamPresentationPosition
(
    _Out_ KSAUDIO_PRESENTATION_POSITION *pPresentationPosition
)
{
    ASSERT (pPresentationPosition);

    // The call must be from event driven mode.
    if(m_ulNotificationsPerBuffer == 0)
    {
        return STATUS_NOT_SUPPORTED;
    }

    return GetPresentationPosition(pPresentationPosition);
}

#pragma code_seg()
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS CMiniportWaveRTStream::GetPacketCount
(
    _Out_ ULONG *pPacketCount
)
{
    ASSERT(pPacketCount);

    // The call must be from event driven mode.
    if(m_ulNotificationsPerBuffer == 0)
    {
        return STATUS_NOT_SUPPORTED;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_PositionSpinLock, &oldIrql);

    if (m_KsState == KSSTATE_RUN)
    {
        LARGE_INTEGER ilQPC = KeQueryPerformanceCounter(NULL);
        UpdatePosition(ilQPC);
    }

    *pPacketCount = LODWORD(m_llPacketCounter);
    KeReleaseSpinLock(&m_PositionSpinLock, oldIrql);

    return STATUS_SUCCESS;
}

// Updates *_pullLinearBufferPosition with bytes fetched from the WaveRT
// buffer since the stream entered RUN state; reads return zero once stopped.
#pragma code_seg()
NTSTATUS CMiniportWaveRTStream::GetPositions(
    _Out_opt_  ULONGLONG* _pullLinearBufferPosition,
    _Out_opt_  ULONGLONG* _pullPresentationPosition,
    _Out_opt_  LARGE_INTEGER* _pliQPCTime
)
{
    DPF_ENTER(("[CMiniportWaveRTStream::GetPositions]"));

    NTSTATUS        ntStatus;
    LARGE_INTEGER   ilQPC;
    KIRQL           oldIrql;

    KeAcquireSpinLock(&m_PositionSpinLock, &oldIrql);
    ilQPC = KeQueryPerformanceCounter(NULL);
    if (m_KsState == KSSTATE_RUN)
    {
        UpdatePosition(ilQPC);
    }
    if (_pullLinearBufferPosition)
    {
        *_pullLinearBufferPosition = m_ullLinearPosition;
    }
    if (_pullPresentationPosition)
    {
        *_pullPresentationPosition = m_ullPresentationPosition;
    }
    KeReleaseSpinLock(&m_PositionSpinLock, oldIrql);
    if (_pliQPCTime)
    {
        *_pliQPCTime = ilQPC;
    }

    ntStatus = STATUS_SUCCESS;

    return ntStatus;
}

NTSTATUS CMiniportWaveRTStream::GetPresentationPosition(_Out_  KSAUDIO_PRESENTATION_POSITION* _pPresentationPosition)
{
    ASSERT(_pPresentationPosition);
    LARGE_INTEGER timeStamp;

    DPF_ENTER(("[CMiniportWaveRTStream::GetPresentationPosition]"));

    ULONGLONG ullLinearPosition = { 0 };
    ULONGLONG ullPresentationPosition = { 0 };
    NTSTATUS status = STATUS_SUCCESS;

    status = GetPositions(&ullLinearPosition, &ullPresentationPosition, &timeStamp);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    _pPresentationPosition->u64PositionInBlocks = ullPresentationPosition * m_pWfExt->Format.nSamplesPerSec / m_pWfExt->Format.nAvgBytesPerSec;
    _pPresentationPosition->u64QPCPosition = (UINT64)timeStamp.QuadPart;

    return STATUS_SUCCESS;
}

#pragma code_seg()
NTSTATUS CMiniportWaveRTStream::SetCurrentWritePositionInternal(_In_  ULONG _ulCurrentWritePosition)
{
    DPF_ENTER(("[CMiniportWaveRTStream::SetCurrentWritePositionInternal]"));

    ASSERT(m_bEoSReceived == FALSE);

    if (m_bEoSReceived)
    {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (_ulCurrentWritePosition > m_ulDmaBufferSize)
    {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    PADAPTERCOMMON pAdapterComm = m_pMiniport->GetAdapterCommObj();

    //Event type: eMINIPORT_SET_WAVERT_BUFFER_WRITE_POSITION
    //Parameter 1: Current linear buffer position
    //Parameter 2: Previous WaveRtBufferWritePosition that the driver received
    //Parameter 3: Target WaveRtBufferWritePosition received from portcls
    //Parameter 4: 0
    pAdapterComm->WriteEtwEvent(eMINIPORT_SET_WAVERT_BUFFER_WRITE_POSITION,
        m_ullLinearPosition, // replace with the correct "Current linear buffer position"
        m_ulCurrentWritePosition,
        _ulCurrentWritePosition, // this is new write position
        0); // always zero

    // Detect eMINIPORT_GLITCH_REPORT - same WaveRT buffer write during event driven mode.
    if (m_ulNotificationIntervalMs > 0)
    {
        if (m_ulCurrentWritePosition == _ulCurrentWritePosition)
        {
            //Event type: eMINIPORT_GLITCH_REPORT
            //Parameter 1: Current linear buffer position
            //Parameter 2: Previous WaveRtBufferWritePosition that the driver received
            //Parameter 3: Major glitch code: 3: Received same WaveRT buffer twice in a row during event driven mode
            //Parameter 4: Minor code for the glitch cause
            pAdapterComm->WriteEtwEvent(eMINIPORT_GLITCH_REPORT,
                m_ullLinearPosition, // replace with the correct "Current linear buffer position"
                m_ulCurrentWritePosition,
                3, // received same WaveRT buffer twice in a row during event driven mode
                _ulCurrentWritePosition);
        }
    }

    m_ulCurrentWritePosition = _ulCurrentWritePosition;
    InterlockedExchange(&m_IsCurrentWritePositionUpdated, 1);

    return STATUS_SUCCESS;
}

#pragma code_seg()
NTSTATUS CMiniportWaveRTStream::SetState
(
    _In_    KSSTATE State_
)
{
    NTSTATUS        ntStatus        = STATUS_SUCCESS;
    KIRQL oldIrql;

    switch (State_)
    {
        case KSSTATE_STOP:
            if (m_KsState == KSSTATE_ACQUIRE)
            {
                // Acquire stream resources
            }
            KeAcquireSpinLock(&m_PositionSpinLock, &oldIrql);
            // Reset DMA
            m_llPacketCounter = 0;
            m_ullPlayPosition = 0;
            m_ullWritePosition = 0;
            m_ullLinearPosition = 0;
            m_ullPresentationPosition = 0;

            // Reset OS read/write positions
            m_ulLastOsReadPacket = ULONG_MAX;
            m_ulCurrentWritePosition = 0;
            m_ulLastOsWritePacket = ULONG_MAX;
            m_bEoSReceived = FALSE;
            m_bLastBufferRendered = FALSE;
            m_ullLoopbackFramesProcessed = 0;

            KeReleaseSpinLock(&m_PositionSpinLock, oldIrql);

            if (!m_bCapture && !g_DoNotCreateDataFiles)
            {
                m_SaveData.WaitAllWorkItems();
            }
            break;

        case KSSTATE_ACQUIRE:
            if (m_KsState == KSSTATE_STOP)
            {
                // Acquire stream resources
            }
            break;

        case KSSTATE_PAUSE:

            if (m_KsState > KSSTATE_PAUSE)
            {
                // Run -> Pause: pause DMA.
                if (m_ulNotificationIntervalMs > 0)
                {
                    ExCancelTimer(m_pNotificationTimer, NULL);
                    KeFlushQueuedDpcs();

                    // Save the time since the last buffer completion event so that if the
                    // pin returns to RUN, the next event fires at the correct time.
                    if (m_ullLastDPCTimeStamp > 0)
                    {
                        LARGE_INTEGER qpc;
                        LARGE_INTEGER qpcFrequency;
                        LONGLONG  hnsCurrentTime;

                        qpc = KeQueryPerformanceCounter(&qpcFrequency);

                        // Convert ticks to 100ns units.
                        hnsCurrentTime = KSCONVERT_PERFORMANCE_TIME(m_ullPerformanceCounterFrequency.QuadPart, qpc);
                        m_hnsDPCTimeCarryForward = hnsCurrentTime - m_ullLastDPCTimeStamp + m_hnsDPCTimeCarryForward;
                    }
                }
            }
            // Updates the linear buffer and presentation positions.
            GetPositions(NULL, NULL, NULL);
            break;

        case KSSTATE_RUN:
            // Start DMA.
            LARGE_INTEGER ullPerfCounterTemp;
            ullPerfCounterTemp = KeQueryPerformanceCounter(&m_ullPerformanceCounterFrequency);
            m_ullLastDPCTimeStamp = m_ullDmaTimeStamp = KSCONVERT_PERFORMANCE_TIME(m_ullPerformanceCounterFrequency.QuadPart, ullPerfCounterTemp);

            if (m_ulNotificationIntervalMs > 0)
            {
                // Timer fires every 1ms (emulating hardware) but notification events are
                // only sent out once the notification interval has elapsed. Real hardware
                // should not poll this fast, since it drains power.
                ExSetTimer
                (
                    m_pNotificationTimer,
                    (-1) * HNSTIME_PER_MILLISECOND,
                    HNSTIME_PER_MILLISECOND, // 1 ms
                    NULL
                 );

            }

            break;
    }

    m_KsState = State_;

    return ntStatus;
}

#pragma code_seg("PAGE")
NTSTATUS CMiniportWaveRTStream::SetFormat
(
    _In_    KSDATAFORMAT    *DataFormat_
)
{
    UNREFERENCED_PARAMETER(DataFormat_);

    PAGED_CODE();

    return STATUS_NOT_SUPPORTED;
}

#pragma code_seg()

#pragma code_seg()
VOID CMiniportWaveRTStream::UpdatePosition
(
    _In_ LARGE_INTEGER ilQPC
)
{
    // Convert ticks to 100ns units.
    LONGLONG  hnsCurrentTime = KSCONVERT_PERFORMANCE_TIME(m_ullPerformanceCounterFrequency.QuadPart, ilQPC);

    // Time elapsed since the last GetPosition() call (or since the DMA engine started).
    // The /10000 to convert to ms loses a remainder, which is carried forward below.
    ULONG TimeElapsedInMS = (ULONG)(hnsCurrentTime - m_ullDmaTimeStamp + m_hnsElapsedTimeCarryForward)/10000;

    m_hnsElapsedTimeCarryForward = (hnsCurrentTime - m_ullDmaTimeStamp + m_hnsElapsedTimeCarryForward) % 10000;

    // Bytes processed in the elapsed time; m_ulDmaMovementRate is bytes/sec so /1000
    // converts via milliseconds, and again the remainder is carried forward.
    ULONG ByteDisplacement = ((m_ulDmaMovementRate * TimeElapsedInMS) + m_byteDisplacementCarryForward) / 1000 ;
    m_byteDisplacementCarryForward = ((m_ulDmaMovementRate * TimeElapsedInMS) + m_byteDisplacementCarryForward) % 1000;

    // Presentation position keeps advancing even after the last buffer is rendered.
    m_ullPresentationPosition += ByteDisplacement;

    if (m_bCapture)
    {
        WriteBytes(ByteDisplacement);
    }
    else
    {
        if (m_bEoSReceived)
        {
            // Don't read data beyond the EoS position, whichever side of it we're currently on.
            if (m_ullWritePosition <= m_ulCurrentWritePosition)
            {
                ByteDisplacement = min(ByteDisplacement, m_ulCurrentWritePosition - (ULONG)m_ullWritePosition);
            }
            else if ((m_ullWritePosition + ByteDisplacement) % m_ulDmaBufferSize < m_ullWritePosition)
            {
                if ((m_ullWritePosition + ByteDisplacement) % m_ulDmaBufferSize > m_ulCurrentWritePosition)
                {
                    ByteDisplacement = ByteDisplacement - (((ULONG)m_ullWritePosition + ByteDisplacement) % m_ulDmaBufferSize - m_ulCurrentWritePosition);
                }
            }
        }

        // If the last packet was rendered (read, in this driver's case), send an ETW event.
        if (m_bEoSReceived && !m_bLastBufferRendered
            && (m_ullWritePosition + ByteDisplacement) % m_ulDmaBufferSize == m_ulCurrentWritePosition)
        {
            m_bLastBufferRendered = TRUE;
        }

        if (!g_DisableLoopback)
        {
            // Push the newly-rendered audio into the speaker->microphone loopback buffer.
            WriteToLoopback(ByteDisplacement);
        }

        if (!g_DoNotCreateDataFiles)
        {
            ReadBytes(ByteDisplacement);
        }
    }

    // Advance the DMA position by the bytes displaced since the last call, wrapping at buffer length.
    m_ullPlayPosition = m_ullWritePosition =
        (m_ullWritePosition + ByteDisplacement) % m_ulDmaBufferSize;

    // m_ullDmaTimeStamp is updated in both GetPosition and GetLinearPosition calls, so
    // m_ullLinearPosition needs the same update here.
    m_ullLinearPosition += ByteDisplacement;

    m_ullDmaTimeStamp = hnsCurrentTime;
}

// Fills the capture buffer with whatever is currently playing on the virtual
// speaker, pulled from the speaker->microphone loopback ring buffer. If
// loopback is disabled, or nothing has ever been rendered yet, this
// naturally degrades to silence.
#pragma code_seg()
VOID CMiniportWaveRTStream::WriteBytes
(
    _In_ ULONG ByteDisplacement
)
{
    ULONG bufferOffset = m_ullLinearPosition % m_ulDmaBufferSize;

    if (g_DisableLoopback || m_pWfExt == NULL || m_pWfExt->Format.nBlockAlign == 0)
    {
        // Loopback disabled (or format not known yet): output silence.
        while (ByteDisplacement > 0)
        {
            ULONG runWrite = min(ByteDisplacement, m_ulDmaBufferSize - bufferOffset);

            RtlZeroMemory(m_pDmaBuffer + bufferOffset, runWrite);

            bufferOffset = (bufferOffset + runWrite) % m_ulDmaBufferSize;
            ByteDisplacement -= runWrite;
        }
        return;
    }

    ULONG      bytesPerFrame    = m_pWfExt->Format.nBlockAlign;
    ULONGLONG  totalFrames      = (m_ullLinearPosition + ByteDisplacement) / bytesPerFrame;
    ULONGLONG  framesToProcess  = totalFrames - m_ullLoopbackFramesProcessed;
    ULONG      loopbackOffset   = (ULONG)((m_ullLoopbackFramesProcessed * bytesPerFrame) % m_ulDmaBufferSize);

    while (framesToProcess > 0)
    {
        ULONG chunkFrames    = (ULONG)min(framesToProcess, (ULONGLONG)LOOPBACK_CHUNK_FRAMES);
        ULONG framesUntilWrap = (m_ulDmaBufferSize - loopbackOffset) / bytesPerFrame;

        if (framesUntilWrap == 0)
        {
            loopbackOffset  = 0;
            framesUntilWrap = m_ulDmaBufferSize / bytesPerFrame;
        }
        if (chunkFrames > framesUntilWrap)
        {
            chunkFrames = framesUntilWrap;
        }
        if (chunkFrames == 0)
        {
            break; // Shouldn't happen: the DMA buffer always holds at least one frame.
        }

        SHORT hubSamples[LOOPBACK_CHUNK_FRAMES * LOOPBACK_HUB_CHANNELS];
        BYTE* pDst = m_pDmaBuffer + loopbackOffset;

        if (m_pWfExt->Format.nSamplesPerSec == LOOPBACK_HUB_SAMPLE_RATE)
        {
            m_pMiniport->GetAdapterCommObj()->LoopbackRead((BYTE*)hubSamples, (ULONG)(chunkFrames * LOOPBACK_HUB_CHANNELS * sizeof(SHORT)));
        }
        else
        {
            ULONG srcFrames = (ULONG)(((ULONGLONG)chunkFrames * LOOPBACK_HUB_SAMPLE_RATE) / m_pWfExt->Format.nSamplesPerSec) + 1;
            if (srcFrames > LOOPBACK_CHUNK_FRAMES)
            {
                srcFrames = LOOPBACK_CHUNK_FRAMES;
            }

            SHORT hubSrc[LOOPBACK_CHUNK_FRAMES * LOOPBACK_HUB_CHANNELS];
            m_pMiniport->GetAdapterCommObj()->LoopbackRead((BYTE*)hubSrc, (ULONG)(srcFrames * LOOPBACK_HUB_CHANNELS * sizeof(SHORT)));
            LoopbackResampleStereo16(hubSrc, srcFrames, LOOPBACK_HUB_SAMPLE_RATE, hubSamples, chunkFrames, m_pWfExt->Format.nSamplesPerSec);
        }

        LoopbackConvertFromHub(hubSamples, chunkFrames, m_pWfExt, pDst);

        loopbackOffset = (loopbackOffset + chunkFrames * bytesPerFrame) % m_ulDmaBufferSize;
        m_ullLoopbackFramesProcessed += chunkFrames;
        framesToProcess -= chunkFrames;
    }
}

// Pushes newly-rendered speaker audio into the speaker -> microphone loopback
// ring buffer (see looprelay.h), converting it from this stream's negotiated
// wave format to the buffer's fixed hub format first.
#pragma code_seg()
VOID CMiniportWaveRTStream::WriteToLoopback
(
    _In_ ULONG ByteDisplacement
)
{
    if (m_pWfExt == NULL || m_pWfExt->Format.nBlockAlign == 0)
    {
        return;
    }

    ULONG      bytesPerFrame    = m_pWfExt->Format.nBlockAlign;
    ULONGLONG  totalFrames      = (m_ullLinearPosition + ByteDisplacement) / bytesPerFrame;
    ULONGLONG  framesToProcess  = totalFrames - m_ullLoopbackFramesProcessed;
    ULONG      loopbackOffset   = (ULONG)((m_ullLoopbackFramesProcessed * bytesPerFrame) % m_ulDmaBufferSize);

    while (framesToProcess > 0)
    {
        ULONG chunkFrames     = (ULONG)min(framesToProcess, (ULONGLONG)LOOPBACK_CHUNK_FRAMES);
        ULONG framesUntilWrap = (m_ulDmaBufferSize - loopbackOffset) / bytesPerFrame;

        if (framesUntilWrap == 0)
        {
            loopbackOffset  = 0;
            framesUntilWrap = m_ulDmaBufferSize / bytesPerFrame;
        }
        if (chunkFrames > framesUntilWrap)
        {
            chunkFrames = framesUntilWrap;
        }
        if (chunkFrames == 0)
        {
            break; // Shouldn't happen: the DMA buffer always holds at least one frame.
        }

        SHORT hubSamples[LOOPBACK_CHUNK_FRAMES * LOOPBACK_HUB_CHANNELS];
        const BYTE* pSrc = m_pDmaBuffer + loopbackOffset;

        LoopbackConvertToHub(pSrc, chunkFrames, m_pWfExt, hubSamples);

        if (m_pWfExt->Format.nSamplesPerSec == LOOPBACK_HUB_SAMPLE_RATE)
        {
            m_pMiniport->GetAdapterCommObj()->LoopbackWrite((BYTE*)hubSamples, (ULONG)(chunkFrames * LOOPBACK_HUB_CHANNELS * sizeof(SHORT)));
        }
        else
        {
            ULONG dstFrames = (ULONG)(((ULONGLONG)chunkFrames * LOOPBACK_HUB_SAMPLE_RATE) / m_pWfExt->Format.nSamplesPerSec) + 1;
            if (dstFrames > LOOPBACK_CHUNK_FRAMES)
            {
                dstFrames = LOOPBACK_CHUNK_FRAMES;
            }

            SHORT hubResampled[LOOPBACK_CHUNK_FRAMES * LOOPBACK_HUB_CHANNELS];
            LoopbackResampleStereo16(hubSamples, chunkFrames, m_pWfExt->Format.nSamplesPerSec, hubResampled, dstFrames, LOOPBACK_HUB_SAMPLE_RATE);
            m_pMiniport->GetAdapterCommObj()->LoopbackWrite((BYTE*)hubResampled, (ULONG)(dstFrames * LOOPBACK_HUB_CHANNELS * sizeof(SHORT)));
        }

        loopbackOffset = (loopbackOffset + chunkFrames * bytesPerFrame) % m_ulDmaBufferSize;
        m_ullLoopbackFramesProcessed += chunkFrames;
        framesToProcess -= chunkFrames;
    }
}

// Reads the audio buffer and saves the data to a debug WAV file on disk.
#pragma code_seg()
VOID CMiniportWaveRTStream::ReadBytes
(
    _In_ ULONG ByteDisplacement
)
{
    ULONG bufferOffset = m_ullLinearPosition % m_ulDmaBufferSize;

    // Normally loops at most once per wrap, but many displaced bytes can mean many iterations.
    while (ByteDisplacement > 0)
    {
        ULONG runWrite = min(ByteDisplacement, m_ulDmaBufferSize - bufferOffset);
        m_SaveData.WriteData(m_pDmaBuffer + bufferOffset, runWrite);
        bufferOffset = (bufferOffset + runWrite) % m_ulDmaBufferSize;
        ByteDisplacement -= runWrite;
    }
}

// Sets the DRM content ID for this stream and asks the miniport to recompute
// the mixed content ID/rights across all active streams.
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CMiniportWaveRTStream::SetContentId
(
    _In_  ULONG                   contentId,
    _In_  PCDRMRIGHTS             drmRights
)
{
    PAGED_CODE();

    DPF_ENTER(("[CMiniportWaveRT::SetContentId]"));

    NTSTATUS    ntStatus;
    ULONG       ulOldContentId = contentId;

    m_ulContentId = contentId;

    ntStatus = m_pMiniport->UpdateDrmRights();

    if (!NT_SUCCESS(ntStatus))
    {
        // Restore the passed-in content ID.
        m_ulContentId = ulOldContentId;
    }

    // Each stream is saved to disk separately; stop writing this one if its
    // rights mark it copy-protected. This driver does not otherwise forward
    // protected content through a validated/authenticated data path - see
    // MSDN's DRM Functions and Interfaces for what a real driver would need
    // (DrmForwardContentToDeviceObject/ToInterfaces, DrmAddContentHandlers).
    m_SaveData.Disable(drmRights->CopyProtect);

    return ntStatus;
} // SetContentId

#pragma code_seg()
void
TimerNotifyRT
(
    _In_      PEX_TIMER    Timer,
    _In_opt_  PVOID        DeferredContext
)
{
    LARGE_INTEGER qpc;
    LARGE_INTEGER qpcFrequency;
    BOOL bufferCompleted = FALSE;

    UNREFERENCED_PARAMETER(Timer);

    _IRQL_limited_to_(DISPATCH_LEVEL);

    CMiniportWaveRTStream* _this = (CMiniportWaveRTStream*)DeferredContext;

    if (NULL == _this)
    {
        return;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&_this->m_PositionSpinLock, &oldIrql);

    qpc = KeQueryPerformanceCounter(&qpcFrequency);

    // Convert ticks to 100ns units.
    LONGLONG  hnsCurrentTime = KSCONVERT_PERFORMANCE_TIME(_this->m_ullPerformanceCounterFrequency.QuadPart, qpc);

    // Time elapsed since the last DPC that matched the notification interval;
    // the /10000 to convert to ms loses a remainder, carried forward below.
    ULONG TimeElapsedInMS = (ULONG)(hnsCurrentTime - _this->m_ullLastDPCTimeStamp + _this->m_hnsDPCTimeCarryForward)/10000;

    if (TimeElapsedInMS >= _this->m_ulNotificationIntervalMs)
    {
        // Carry forward time beyond the notification interval so the next
        // buffer-completion event still fires at the right moment.
        _this->m_hnsDPCTimeCarryForward = hnsCurrentTime - _this->m_ullLastDPCTimeStamp + _this->m_hnsDPCTimeCarryForward - (_this->m_ulNotificationIntervalMs * 10000);
        _this->m_ullLastDPCTimeStamp = hnsCurrentTime;
        bufferCompleted = TRUE;
    }

    if (!bufferCompleted && !_this->m_bEoSReceived)
    {
        goto End;
    }

    _this->UpdatePosition(qpc);

    if (!_this->m_bEoSReceived)
    {
        _this->m_llPacketCounter++;
    }

    if (_this->m_KsState != KSSTATE_RUN)
    {
        goto End;
    }

    PADAPTERCOMMON  pAdapterComm = _this->m_pMiniport->GetAdapterCommObj();

    // Simple buffer underrun detection.
    if (!_this->IsCurrentWaveRTWritePositionUpdated() && !_this->m_bEoSReceived)
    {
        //Event type: eMINIPORT_GLITCH_REPORT
        //Parameter 1: Current linear buffer position
        //Parameter 2: Previous WaveRtBufferWritePosition that the driver received
        //Parameter 3: Major glitch code: 1:WaveRT buffer is underrun
        //Parameter 4: Minor code for the glitch cause
        pAdapterComm->WriteEtwEvent(eMINIPORT_GLITCH_REPORT,
                                    _this->m_ullLinearPosition,
                                    _this->GetCurrentWaveRTWritePosition(),
                                    1,      // WaveRT buffer is underrun
                                    0);
    }

    // Send the buffer completion event if either a full buffer completed, or a
    // partial buffer containing EoS did.
    if (!IsListEmpty(&_this->m_NotificationList) &&
        (bufferCompleted || _this->m_bLastBufferRendered))
    {
        PLIST_ENTRY leCurrent = _this->m_NotificationList.Flink;
        while (leCurrent != &_this->m_NotificationList)
        {
            NotificationListEntry* nleCurrent = CONTAINING_RECORD( leCurrent, NotificationListEntry, ListEntry);
            KeSetEvent(nleCurrent->NotificationEvent, 0, 0);

            leCurrent = leCurrent->Flink;
        }
    }

    if (_this->m_bLastBufferRendered)
    {
        ExCancelTimer(_this->m_pNotificationTimer, NULL);
    }

End:
    KeReleaseSpinLock(&_this->m_PositionSpinLock, oldIrql);
    return;
}
