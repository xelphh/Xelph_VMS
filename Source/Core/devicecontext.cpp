/*++

Copyright (c) Xelph. All Rights Reserved.

Module Name:

    devicecontext.cpp

Abstract:

    Implementation of CAdapterCommon (IAdapterCommon/IAdapterPowerManagement),
    the adapter-wide context object shared by every endpoint on this device.
    Owns the mixer HW state (CMixerState), the subdevice cache used to
    install/reuse the wave+topology port/miniport pairs for each endpoint
    (InstallSubdevice/InstallEndpointFilters/GetFilters), device-interface
    registration and property stamping, bridge-pin connections between wave
    and topology filters, adapter power-state management, registry-key
    copy helpers used for endpoint template migration, and the speaker ->
    microphone loopback ring buffer (LoopbackWrite/LoopbackRead), which is
    what streamworker.cpp actually pushes/pulls PCM through.

Contact:

    Discord: xelphh
    Website: https://Xelph.lol

--*/

#pragma warning (disable : 4127)

#include <initguid.h>
#include "shared.h"
#include "mixerstate.h"
#include "wavewriter.h"
#include "pinmap.h"
#include "looprelay.h"

PSAVEWORKER_PARAM       CSaveData::m_pWorkItems = NULL;
PDEVICE_OBJECT          CSaveData::m_pDeviceObject = NULL;

class CAdapterCommon :
    public IAdapterCommon,
    public IAdapterPowerManagement,
    public CUnknown
{
    private:
        PSERVICEGROUP           m_pServiceGroupWave;
        PDEVICE_OBJECT          m_pDeviceObject;
        PDEVICE_OBJECT          m_pPhysicalDeviceObject;
        WDFDEVICE               m_WdfDevice;            // Wdf device.
        DEVICE_POWER_STATE      m_PowerState;

        PCMixerState   m_pHW;                  // Virtual Simple Audio Sample HW object
        PPORTCLSETWHELPER       m_pPortClsEtwHelper;
        CLoopbackRingBuffer     m_LoopbackBuffer;       // Speaker -> microphone loopback

        static LONG             m_AdapterInstances;     // # of adapter objects.

        DWORD                   m_dwIdleRequests;

    public:
        DECLARE_STD_UNKNOWN();
        DEFINE_STD_CONSTRUCTOR(CAdapterCommon);
        ~CAdapterCommon();

        IMP_IAdapterPowerManagement;

        STDMETHODIMP_(NTSTATUS) Init
        (
            _In_  PDEVICE_OBJECT  DeviceObject
        );

        STDMETHODIMP_(PDEVICE_OBJECT)   GetDeviceObject(void);

        STDMETHODIMP_(PDEVICE_OBJECT)   GetPhysicalDeviceObject(void);

        STDMETHODIMP_(WDFDEVICE)        GetWdfDevice(void);

        STDMETHODIMP_(void)     SetWaveServiceGroup
        (
            _In_  PSERVICEGROUP   ServiceGroup
        );

        STDMETHODIMP_(BOOL)     bDevSpecificRead();

        STDMETHODIMP_(void)     bDevSpecificWrite
        (
            _In_  BOOL            bDevSpecific
        );
        STDMETHODIMP_(INT)      iDevSpecificRead();

        STDMETHODIMP_(void)     iDevSpecificWrite
        (
            _In_  INT             iDevSpecific
        );
        STDMETHODIMP_(UINT)     uiDevSpecificRead();

        STDMETHODIMP_(void)     uiDevSpecificWrite
        (
            _In_  UINT            uiDevSpecific
        );

        STDMETHODIMP_(BOOL)     MixerMuteRead
        (
            _In_  ULONG           Index,
            _In_  ULONG           Channel
        );

        STDMETHODIMP_(void)     MixerMuteWrite
        (
            _In_  ULONG           Index,
            _In_  ULONG           Channel,
            _In_  BOOL            Value
        );

        STDMETHODIMP_(ULONG)    MixerMuxRead(void);

        STDMETHODIMP_(void)     MixerMuxWrite
        (
            _In_  ULONG           Index
        );

        STDMETHODIMP_(void)     MixerReset(void);

        STDMETHODIMP_(LONG)     MixerVolumeRead
        (
            _In_  ULONG           Index,
            _In_  ULONG           Channel
        );

        STDMETHODIMP_(void)     MixerVolumeWrite
        (
            _In_  ULONG           Index,
            _In_  ULONG           Channel,
            _In_  LONG            Value
        );

        STDMETHODIMP_(LONG)     MixerPeakMeterRead
        (
            _In_  ULONG           Index,
            _In_  ULONG           Channel
        );

        STDMETHODIMP_(NTSTATUS) WriteEtwEvent
        (
            _In_ EPcMiniportEngineEvent    miniportEventType,
            _In_ ULONGLONG      ullData1,
            _In_ ULONGLONG      ullData2,
            _In_ ULONGLONG      ullData3,
            _In_ ULONGLONG      ullData4
        );

        STDMETHODIMP_(VOID)     SetEtwHelper
        (
            PPORTCLSETWHELPER _pPortClsEtwHelper
        );

        STDMETHODIMP_(NTSTATUS) InstallSubdevice
        (
            _In_opt_        PIRP                                        Irp,
            _In_            PWSTR                                       Name,
            _In_opt_        PWSTR                                       TemplateName,
            _In_            REFGUID                                     PortClassId,
            _In_            REFGUID                                     MiniportClassId,
            _In_opt_        PFNCREATEMINIPORT                           MiniportCreate,
            _In_            ULONG                                       cPropertyCount,
            _In_reads_opt_(cPropertyCount) const AUDIO_DEVPROPERTY   * pProperties,
            _In_opt_        PVOID                                       DeviceContext,
            _In_            PENDPOINT_MINIPAIR                          MiniportPair,
            _In_opt_        PRESOURCELIST                               ResourceList,
            _In_            REFGUID                                     PortInterfaceId,
            _Out_opt_       PUNKNOWN                                  * OutPortInterface,
            _Out_opt_       PUNKNOWN                                  * OutPortUnknown,
            _Out_opt_       PUNKNOWN                                  * OutMiniportUnknown
        );

        STDMETHODIMP_(NTSTATUS) UnregisterSubdevice
        (
            _In_opt_ PUNKNOWN               UnknownPort
        );

        STDMETHODIMP_(NTSTATUS) ConnectTopologies
        (
            _In_ PUNKNOWN                   UnknownTopology,
            _In_ PUNKNOWN                   UnknownWave,
            _In_ PHYSICALCONNECTIONTABLE*   PhysicalConnections,
            _In_ ULONG                      PhysicalConnectionCount
        );

        STDMETHODIMP_(NTSTATUS) DisconnectTopologies
        (
            _In_ PUNKNOWN                   UnknownTopology,
            _In_ PUNKNOWN                   UnknownWave,
            _In_ PHYSICALCONNECTIONTABLE*   PhysicalConnections,
            _In_ ULONG                      PhysicalConnectionCount
        );

        STDMETHODIMP_(NTSTATUS) InstallEndpointFilters
        (
            _In_opt_    PIRP                Irp,
            _In_        PENDPOINT_MINIPAIR  MiniportPair,
            _In_opt_    PVOID               DeviceContext,
            _Out_opt_   PUNKNOWN *          UnknownTopology,
            _Out_opt_   PUNKNOWN *          UnknownWave,
            _Out_opt_   PUNKNOWN *          UnknownMiniportTopology,
            _Out_opt_   PUNKNOWN *          UnknownMiniportWave
        );

        STDMETHODIMP_(NTSTATUS) RemoveEndpointFilters
        (
            _In_        PENDPOINT_MINIPAIR  MiniportPair,
            _In_opt_    PUNKNOWN            UnknownTopology,
            _In_opt_    PUNKNOWN            UnknownWave
        );

        STDMETHODIMP_(NTSTATUS) GetFilters
        (
            _In_        PENDPOINT_MINIPAIR  MiniportPair,
            _Out_opt_   PUNKNOWN            *UnknownTopologyPort,
            _Out_opt_   PUNKNOWN            *UnknownTopologyMiniport,
            _Out_opt_   PUNKNOWN            *UnknownWavePort,
            _Out_opt_   PUNKNOWN            *UnknownWaveMiniport
        );

        STDMETHODIMP_(NTSTATUS) SetIdlePowerManagement
        (
            _In_        PENDPOINT_MINIPAIR  MiniportPair,
            _In_        BOOL                bEnabled
        );

        STDMETHODIMP_(VOID) LoopbackWrite
        (
            _In_reads_bytes_(cbData) const BYTE* pData,
            _In_ ULONG                           cbData
        );

        STDMETHODIMP_(VOID) LoopbackRead
        (
            _Out_writes_bytes_(cbRequested) BYTE* pBuffer,
            _In_ ULONG                            cbRequested
        );

        STDMETHODIMP_(VOID) Cleanup();

        friend NTSTATUS         NewAdapterCommon
        (
            _Out_       PUNKNOWN *              Unknown,
            _In_        REFCLSID,
            _In_opt_    PUNKNOWN                UnknownOuter,
            _In_        POOL_FLAGS              PoolFlags
        );

    private:

    LIST_ENTRY m_SubdeviceCache;

    NTSTATUS GetCachedSubdevice
    (
        _In_ PWSTR Name,
        _Out_opt_ PUNKNOWN *OutUnknownPort,
        _Out_opt_ PUNKNOWN *OutUnknownMiniport
    );

    NTSTATUS CacheSubdevice
    (
        _In_ PWSTR Name,
        _In_ PUNKNOWN UnknownPort,
        _In_ PUNKNOWN UnknownMiniport
    );

    NTSTATUS RemoveCachedSubdevice
    (
        _In_ PWSTR Name
    );

    VOID EmptySubdeviceCache();

    NTSTATUS CreateAudioInterfaceWithProperties
    (
        _In_ PCWSTR                                                 ReferenceString,
        _In_opt_ PCWSTR                                             TemplateReferenceString,
        _In_ ULONG                                                  cPropertyCount,
        _In_reads_opt_(cPropertyCount) const AUDIO_DEVPROPERTY        *pProperties,
        _Out_ _At_(AudioSymbolicLinkName->Buffer, __drv_allocatesMem(Mem)) PUNICODE_STRING AudioSymbolicLinkName
    );

    NTSTATUS MigrateDeviceInterfaceTemplateParameters
    (
        _In_ PUNICODE_STRING    SymbolicLinkName,
        _In_opt_ PCWSTR         TemplateReferenceString
    );
};

typedef struct _MINIPAIR_UNKNOWN
{
    LIST_ENTRY              ListEntry;
    WCHAR                   Name[MAX_PATH];
    PUNKNOWN                PortInterface;
    PUNKNOWN                MiniportInterface;
    PADAPTERPOWERMANAGEMENT PowerInterface;
} MINIPAIR_UNKNOWN;

#define MAX_DEVICE_REG_KEY_LENGTH 0x100

//
// Used to implement the singleton pattern.
//
LONG  CAdapterCommon::m_AdapterInstances = 0;

#pragma code_seg("PAGE")
NTSTATUS SetDeviceInterfacePropertiesMultiple
(
    _In_ PUNICODE_STRING                                        SymbolicLinkName,
    _In_ ULONG                                                  cPropertyCount,
    _In_reads_opt_(cPropertyCount) const AUDIO_DEVPROPERTY        *pProperties
)
{
    NTSTATUS ntStatus;

    PAGED_CODE();

    if (pProperties)
    {
        for (ULONG i = 0; i < cPropertyCount; i++)
        {
            ntStatus = IoSetDeviceInterfacePropertyData(
                SymbolicLinkName,
                pProperties[i].PropertyKey,
                LOCALE_NEUTRAL,
                PLUGPLAY_PROPERTY_PERSISTENT,
                pProperties[i].Type,
                pProperties[i].BufferSize,
                pProperties[i].Buffer);

            if (!NT_SUCCESS(ntStatus))
            {
                return ntStatus;
            }
        }
    }

    return STATUS_SUCCESS;
}

// This driver supports only a single instance (CSaveData's static members and
// the Bluetooth HFP logic both assume a singleton adapter).
#pragma code_seg("PAGE")
NTSTATUS
NewAdapterCommon
(
    _Out_       PUNKNOWN *              Unknown,
    _In_        REFCLSID,
    _In_opt_    PUNKNOWN                UnknownOuter,
    _In_        POOL_FLAGS               PoolFlags
)
{
    PAGED_CODE();

    ASSERT(Unknown);

    NTSTATUS ntStatus;

    if (InterlockedCompareExchange(&CAdapterCommon::m_AdapterInstances, 1, 0) != 0)
    {
        ntStatus = STATUS_DEVICE_BUSY;
        DPF(D_ERROR, ("NewAdapterCommon failed, only one instance is allowed"));
        goto Done;
    }

    CAdapterCommon *p = new(PoolFlags, MINADAPTER_POOLTAG) CAdapterCommon(UnknownOuter);
    if (p == NULL)
    {
        ntStatus = STATUS_INSUFFICIENT_RESOURCES;
        DPF(D_ERROR, ("NewAdapterCommon failed, 0x%x", ntStatus));
        goto Done;
    }

    *Unknown = PUNKNOWN((PADAPTERCOMMON)(p));
    (*Unknown)->AddRef();
    ntStatus = STATUS_SUCCESS;

Done:
    return ntStatus;
} // NewAdapterCommon

#pragma code_seg("PAGE")
CAdapterCommon::~CAdapterCommon
(
    void
)
{
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::~CAdapterCommon]"));

    if (m_pHW)
    {
        delete m_pHW;
        m_pHW = NULL;
    }

    CSaveData::DestroyWorkItems();
    SAFE_RELEASE(m_pPortClsEtwHelper);
    SAFE_RELEASE(m_pServiceGroupWave);

    if (m_WdfDevice)
    {
        WdfObjectDelete(m_WdfDevice);
        m_WdfDevice = NULL;
    }

    InterlockedDecrement(&CAdapterCommon::m_AdapterInstances);
    ASSERT(CAdapterCommon::m_AdapterInstances == 0);
} // ~CAdapterCommon

#pragma code_seg("PAGE")
STDMETHODIMP_(PDEVICE_OBJECT)
CAdapterCommon::GetDeviceObject
(
    void
)
{
    PAGED_CODE();

    return m_pDeviceObject;
} // GetDeviceObject

#pragma code_seg("PAGE")
STDMETHODIMP_(PDEVICE_OBJECT)
CAdapterCommon::GetPhysicalDeviceObject
(
    void
)
{
    PAGED_CODE();

    return m_pPhysicalDeviceObject;
} // GetPhysicalDeviceObject

// Returns the WDF device associated with the adapter - note this is NOT an
// audio miniport, just the WDF miniport device used for Bluetooth SCO HFP.
#pragma code_seg("PAGE")
STDMETHODIMP_(WDFDEVICE)
CAdapterCommon::GetWdfDevice
(
    void
)
{
    PAGED_CODE();

    return m_WdfDevice;
} // GetWdfDevice

#pragma code_seg("PAGE")
NTSTATUS
CAdapterCommon::Init
(
    _In_  PDEVICE_OBJECT          DeviceObject
)
{
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::Init]"));

    ASSERT(DeviceObject);

    NTSTATUS        ntStatus    = STATUS_SUCCESS;

    m_pServiceGroupWave     = NULL;
    m_pDeviceObject         = DeviceObject;
    m_pPhysicalDeviceObject = NULL;
    m_WdfDevice             = NULL;
    m_PowerState            = PowerDeviceD0;
    m_pHW                   = NULL;
    m_pPortClsEtwHelper     = NULL;

    InitializeListHead(&m_SubdeviceCache);

    ntStatus = PcGetPhysicalDeviceObject(DeviceObject, &m_pPhysicalDeviceObject);
    IF_FAILED_ACTION_JUMP(
        ntStatus,
        DPF(D_ERROR, ("PcGetPhysicalDeviceObject failed, 0x%x", ntStatus)),
        Done);

    // Create a WDF miniport to represent the adapter. Note that WDF miniports
    // are NOT audio miniports. An audio adapter is associated with a single WDF
    // miniport. This driver uses WDF to simplify the handling of the Bluetooth
    // SCO HFP Bypass interface.
    ntStatus = WdfDeviceMiniportCreate( WdfGetDriver(),
                                        WDF_NO_OBJECT_ATTRIBUTES,
                                        DeviceObject,           // FDO
                                        NULL,                   // Next device.
                                        NULL,                   // PDO
                                       &m_WdfDevice);
    IF_FAILED_ACTION_JUMP(
        ntStatus,
        DPF(D_ERROR, ("WdfDeviceMiniportCreate failed, 0x%x", ntStatus)),
        Done);

    m_pHW = new (POOL_FLAG_NON_PAGED, DRIVER_POOLTAG)  CMixerState;
    if (!m_pHW)
    {
        DPF(D_TERSE, ("Insufficient memory for Simple Audio Sample HW"));
        ntStatus = STATUS_INSUFFICIENT_RESOURCES;
    }
    IF_FAILED_JUMP(ntStatus, Done);

    m_pHW->MixerReset();

    ntStatus = m_LoopbackBuffer.Init(LOOPBACK_BUFFER_DURATION_MS);
    IF_FAILED_JUMP(ntStatus, Done);

    CSaveData::SetDeviceObject(DeviceObject);   //device object is needed by CSaveData
    ntStatus = CSaveData::InitializeWorkItems(DeviceObject);
    IF_FAILED_JUMP(ntStatus, Done);
Done:

    return ntStatus;
} // Init

#pragma code_seg("PAGE")
STDMETHODIMP_(void)
CAdapterCommon::MixerReset
(
    void
)
{
    PAGED_CODE();

    if (m_pHW)
    {
        m_pHW->MixerReset();
    }
} // MixerReset

// Pushes newly-rendered speaker PCM (hub format - see looprelay.h) into the
// speaker -> microphone loopback ring buffer. Callable at up to
// DISPATCH_LEVEL.
#pragma code_seg()
STDMETHODIMP_(VOID)
CAdapterCommon::LoopbackWrite
(
    _In_reads_bytes_(cbData) const BYTE* pData,
    _In_ ULONG                           cbData
)
{
    m_LoopbackBuffer.Write(pData, cbData);
} // LoopbackWrite

// Pulls PCM (hub format - see looprelay.h) out of the speaker -> microphone
// loopback ring buffer for the microphone stream to consume. Any shortfall
// is silence-filled. Callable at up to DISPATCH_LEVEL.
#pragma code_seg()
STDMETHODIMP_(VOID)
CAdapterCommon::LoopbackRead
(
    _Out_writes_bytes_(cbRequested) BYTE* pBuffer,
    _In_ ULONG                            cbRequested
)
{
    m_LoopbackBuffer.Read(pBuffer, cbRequested);
} // LoopbackRead

/* Standard miniport ETW event definitions:

Event type  : eMINIPORT_IHV_DEFINED
Parameter 1 : Defined and used by IHVs
Parameter 2 : Defined and used by IHVs
Parameter 3 : Defined and used by IHVs
Parameter 4 :Defined and used by IHVs

Event type: eMINIPORT_BUFFER_COMPLETE
Parameter 1: Current linear buffer position
Parameter 2: the previous WaveRtBufferWritePosition that the drive received
Parameter 3: Data length completed
Parameter 4:0

Event type: eMINIPORT_PIN_STATE
Parameter 1: Current linear buffer position
Parameter 2: the previous WaveRtBufferWritePosition that the drive received
Parameter 3: Pin State 0->KS_STOP, 1->KS_ACQUIRE, 2->KS_PAUSE, 3->KS_RUN
Parameter 4:0

Event type: eMINIPORT_GET_STREAM_POS
Parameter 1: Current linear buffer position
Parameter 2: the previous WaveRtBufferWritePosition that the drive received
Parameter 3: 0
Parameter 4:0


Event type: eMINIPORT_SET_WAVERT_BUFFER_WRITE_POS
Parameter 1: Current linear buffer position
Parameter 2: the previous WaveRtBufferWritePosition that the drive received
Parameter 3: the arget WaveRtBufferWritePosition received from portcls
Parameter 4:0

Event type: eMINIPORT_GET_PRESENTATION_POS
Parameter 1: Current linear buffer position
Parameter 2: the previous WaveRtBufferWritePosition that the drive received
Parameter 3: Presentation position
Parameter 4:0

Event type: eMINIPORT_PROGRAM_DMA
Parameter 1: Current linear buffer position
Parameter 2: the previous WaveRtBufferWritePosition that the drive received
Parameter 3: Starting  WaveRt buffer offset
Parameter 4: Data length

Event type: eMINIPORT_GLITCH_REPORT
Parameter 1: Current linear buffer position
Parameter 2: the previous WaveRtBufferWritePosition that the drive received
Parameter 3: major glitch code: 1:WaveRT buffer is underrun,
                                2:decoder errors,
                                3:receive the same wavert buffer two in a row in event driven mode
Parameter 4: minor code for the glitch cause

Event type: eMINIPORT_LAST_BUFFER_RENDERED
Parameter 1: Current linear buffer position
Parameter 2: the very last WaveRtBufferWritePosition that the driver received
Parameter 3: 0
Parameter 4: 0

*/
#pragma code_seg()
STDMETHODIMP
CAdapterCommon::WriteEtwEvent
(
    _In_ EPcMiniportEngineEvent    miniportEventType,
    _In_ ULONGLONG  ullData1,
    _In_ ULONGLONG  ullData2,
    _In_ ULONGLONG  ullData3,
    _In_ ULONGLONG  ullData4
)
{
    NTSTATUS ntStatus = STATUS_SUCCESS;

    if (m_pPortClsEtwHelper)
    {
        ntStatus = m_pPortClsEtwHelper->MiniportWriteEtwEvent( miniportEventType, ullData1, ullData2, ullData3, ullData4) ;
    }
    return ntStatus;
} // WriteEtwEvent

#pragma code_seg("PAGE")
STDMETHODIMP_(void)
CAdapterCommon::SetEtwHelper
(
    PPORTCLSETWHELPER _pPortClsEtwHelper
)
{
    PAGED_CODE();

    SAFE_RELEASE(m_pPortClsEtwHelper);

    m_pPortClsEtwHelper = _pPortClsEtwHelper;

    if (m_pPortClsEtwHelper)
    {
        m_pPortClsEtwHelper->AddRef();
    }
} // SetEtwHelper

#pragma code_seg("PAGE")
STDMETHODIMP
CAdapterCommon::NonDelegatingQueryInterface
(
    _In_ REFIID                      Interface,
    _COM_Outptr_ PVOID *        Object
)
{
    PAGED_CODE();

    ASSERT(Object);

    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
    {
        *Object = PVOID(PUNKNOWN(PADAPTERCOMMON(this)));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IAdapterCommon))
    {
        *Object = PVOID(PADAPTERCOMMON(this));
    }
    else if (IsEqualGUIDAligned(Interface, IID_IAdapterPowerManagement))
    {
        *Object = PVOID(PADAPTERPOWERMANAGEMENT(this));
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
STDMETHODIMP_(void)
CAdapterCommon::SetWaveServiceGroup
(
    _In_ PSERVICEGROUP            ServiceGroup
)
{
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::SetWaveServiceGroup]"));

    SAFE_RELEASE(m_pServiceGroupWave);

    m_pServiceGroupWave = ServiceGroup;

    if (m_pServiceGroupWave)
    {
        m_pServiceGroupWave->AddRef();
    }
} // SetWaveServiceGroup

#pragma code_seg()
STDMETHODIMP_(BOOL)
CAdapterCommon::bDevSpecificRead()
{
    if (m_pHW)
    {
        return m_pHW->bGetDevSpecific();
    }

    return FALSE;
} // bDevSpecificRead

#pragma code_seg()
STDMETHODIMP_(void)
CAdapterCommon::bDevSpecificWrite
(
    _In_  BOOL                    bDevSpecific
)
{
    if (m_pHW)
    {
        m_pHW->bSetDevSpecific(bDevSpecific);
    }
} // DevSpecificWrite

#pragma code_seg()
STDMETHODIMP_(INT)
CAdapterCommon::iDevSpecificRead()
{
    if (m_pHW)
    {
        return m_pHW->iGetDevSpecific();
    }

    return 0;
} // iDevSpecificRead

#pragma code_seg()
STDMETHODIMP_(void)
CAdapterCommon::iDevSpecificWrite
(
    _In_  INT                    iDevSpecific
)
{
    if (m_pHW)
    {
        m_pHW->iSetDevSpecific(iDevSpecific);
    }
} // iDevSpecificWrite

#pragma code_seg()
STDMETHODIMP_(UINT)
CAdapterCommon::uiDevSpecificRead()
{
    if (m_pHW)
    {
        return m_pHW->uiGetDevSpecific();
    }

    return 0;
} // uiDevSpecificRead

#pragma code_seg()
STDMETHODIMP_(void)
CAdapterCommon::uiDevSpecificWrite
(
    _In_  UINT                    uiDevSpecific
)
{
    if (m_pHW)
    {
        m_pHW->uiSetDevSpecific(uiDevSpecific);
    }
} // uiDevSpecificWrite

#pragma code_seg()
STDMETHODIMP_(BOOL)
CAdapterCommon::MixerMuteRead
(
    _In_  ULONG               Index,
    _In_  ULONG               Channel
)
{
    if (m_pHW)
    {
        return m_pHW->GetMixerMute(Index, Channel);
    }

    return 0;
} // MixerMuteRead

#pragma code_seg()
STDMETHODIMP_(void)
CAdapterCommon::MixerMuteWrite
(
    _In_  ULONG                   Index,
    _In_  ULONG                   Channel,
    _In_  BOOL                    Value
)
{
    if (m_pHW)
    {
        m_pHW->SetMixerMute(Index, Channel, Value);
    }
} // MixerMuteWrite

#pragma code_seg()
STDMETHODIMP_(ULONG)
CAdapterCommon::MixerMuxRead()
{
    if (m_pHW)
    {
        return m_pHW->GetMixerMux();
    }

    return 0;
} // MixerMuxRead

#pragma code_seg()
STDMETHODIMP_(void)
CAdapterCommon::MixerMuxWrite
(
    _In_  ULONG                   Index
)
{
    if (m_pHW)
    {
        m_pHW->SetMixerMux(Index);
    }
} // MixerMuxWrite

#pragma code_seg()
STDMETHODIMP_(LONG)
CAdapterCommon::MixerVolumeRead
(
    _In_  ULONG                   Index,
    _In_  ULONG                   Channel
)
{
    if (m_pHW)
    {
        return m_pHW->GetMixerVolume(Index, Channel);
    }

    return 0;
} // MixerVolumeRead

#pragma code_seg()
STDMETHODIMP_(void)
CAdapterCommon::MixerVolumeWrite
(
    _In_  ULONG                   Index,
    _In_  ULONG                   Channel,
    _In_  LONG                    Value
)
{
    if (m_pHW)
    {
        m_pHW->SetMixerVolume(Index, Channel, Value);
    }
} // MixerVolumeWrite

#pragma code_seg()
STDMETHODIMP_(LONG)
CAdapterCommon::MixerPeakMeterRead
(
    _In_  ULONG                   Index,
    _In_  ULONG                   Channel
)
{
    if (m_pHW)
    {
        return m_pHW->GetMixerPeakMeter(Index, Channel);
    }

    return 0;
} // MixerVolumeRead

// PortCls pauses active streams before calling this to sleep the device, and
// unpauses them afterward to wake it. The driver must complete the power
// change before returning, and must not touch hardware while in any state
// other than PowerDeviceD0 - PortCls always restores D0 before creating a
// miniport or stream object.
#pragma code_seg()
STDMETHODIMP_(void)
CAdapterCommon::PowerChangeState
(
    _In_  POWER_STATE             NewState
)
{
    DPF_ENTER(("[CAdapterCommon::PowerChangeState]"));

    // Notify all registered miniports of a power state change
    PLIST_ENTRY le = NULL;
    for (le = m_SubdeviceCache.Flink; le != &m_SubdeviceCache; le = le->Flink)
    {
        MINIPAIR_UNKNOWN *pRecord = CONTAINING_RECORD(le, MINIPAIR_UNKNOWN, ListEntry);

        if (pRecord->PowerInterface)
        {
            pRecord->PowerInterface->PowerChangeState(NewState);
        }
    }

    if (NewState.DeviceState != m_PowerState)
    {
        switch (NewState.DeviceState)
        {
            case PowerDeviceD0:
            case PowerDeviceD1:
            case PowerDeviceD2:
            case PowerDeviceD3:
                m_PowerState = NewState.DeviceState;

                DPF
                (
                    D_VERBOSE,
                    ("Entering D%u", ULONG(m_PowerState) - ULONG(PowerDeviceD0))
                );

                break;

            default:

                DPF(D_VERBOSE, ("Unknown Device Power State"));
                break;
        }
    }
} // PowerStateChange

// Provides the system-to-device power state mapping at startup; standard
// across adapters and typically doesn't need modification.
#pragma code_seg()
STDMETHODIMP_(NTSTATUS)
CAdapterCommon::QueryDeviceCapabilities
(
    _Inout_updates_bytes_(sizeof(DEVICE_CAPABILITIES)) PDEVICE_CAPABILITIES    PowerDeviceCaps
)
{
    UNREFERENCED_PARAMETER(PowerDeviceCaps);

    DPF_ENTER(("[CAdapterCommon::QueryDeviceCapabilities]"));

    return (STATUS_SUCCESS);
} // QueryDeviceCapabilities

#pragma code_seg()
STDMETHODIMP_(NTSTATUS)
CAdapterCommon::QueryPowerChangeState
(
    _In_  POWER_STATE             NewStateQuery
)
{
    NTSTATUS status = STATUS_SUCCESS;

    DPF_ENTER(("[CAdapterCommon::QueryPowerChangeState]"));

    // Query each miniport for its power state; fail as soon as one refuses this state.
    PLIST_ENTRY le = NULL;
    for (le = m_SubdeviceCache.Flink; le != &m_SubdeviceCache && NT_SUCCESS(status); le = le->Flink)
    {
        MINIPAIR_UNKNOWN *pRecord = CONTAINING_RECORD(le, MINIPAIR_UNKNOWN, ListEntry);

        if (pRecord->PowerInterface)
        {
            status = pRecord->PowerInterface->QueryPowerChangeState(NewStateQuery);
        }
    }

    return status;
} // QueryPowerChangeState

// Registers the audio interface (in disabled mode).
#pragma code_seg("PAGE")
NTSTATUS
CAdapterCommon::CreateAudioInterfaceWithProperties
(
    _In_ PCWSTR ReferenceString,
    _In_opt_ PCWSTR TemplateReferenceString,
    _In_ ULONG cPropertyCount,
    _In_reads_opt_(cPropertyCount) const AUDIO_DEVPROPERTY *pProperties,
    _Out_ _At_(AudioSymbolicLinkName->Buffer, __drv_allocatesMem(Mem)) PUNICODE_STRING AudioSymbolicLinkName
)
{
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::CreateAudioInterfaceWithProperties]"));

    NTSTATUS        ntStatus;
    UNICODE_STRING  referenceString;

    RtlInitUnicodeString(&referenceString, ReferenceString);

    RtlZeroMemory(AudioSymbolicLinkName, sizeof(UNICODE_STRING));

    ntStatus = IoRegisterDeviceInterface(
        GetPhysicalDeviceObject(),
        &KSCATEGORY_AUDIO,
        &referenceString,
        AudioSymbolicLinkName);

    IF_FAILED_ACTION_JUMP(
        ntStatus,
        DPF(D_ERROR, ("CreateAudioInterfaceWithProperties: IoRegisterDeviceInterface(KSCATEGORY_AUDIO): failed, 0x%x", ntStatus)),
        Done);

    // Migrate optional device interface parameters from the template first, if
    // present, so any additional parameters in pProperties can override the defaults.
    if (NULL != TemplateReferenceString)
    {
        ntStatus = MigrateDeviceInterfaceTemplateParameters(AudioSymbolicLinkName, TemplateReferenceString);

        IF_FAILED_ACTION_JUMP(
            ntStatus,
            DPF(D_ERROR, ("MigrateDeviceInterfaceTempalteParameters: MigrateDeviceInterfaceTemplateParameters(...): failed, 0x%x", ntStatus)),
            Done);
    }

    ntStatus = SetDeviceInterfacePropertiesMultiple(AudioSymbolicLinkName, cPropertyCount, pProperties);

    IF_FAILED_ACTION_JUMP(
        ntStatus,
        DPF(D_ERROR, ("CreateAudioInterfaceWithProperties: SetDeviceInterfacePropertiesMultiple(...): failed, 0x%x", ntStatus)),
        Done);

    ntStatus = STATUS_SUCCESS;

Done:
    if (!NT_SUCCESS(ntStatus))
    {
        RtlFreeUnicodeString(AudioSymbolicLinkName);
        RtlZeroMemory(AudioSymbolicLinkName, sizeof(UNICODE_STRING));
    }
    return ntStatus;
}

// Creates and registers a subdevice: a port driver + miniport driver + a set
// of resources bound together, optionally depositing a pointer to the port
// driver's interface at OutPortInterface before Init, so a common ISR can
// reach the port driver during its own initialization if it fires early.
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CAdapterCommon::InstallSubdevice
(
    _In_opt_        PIRP                                    Irp,
    _In_            PWSTR                                   Name,
    _In_opt_        PWSTR                                   TemplateName,
    _In_            REFGUID                                 PortClassId,
    _In_            REFGUID                                 MiniportClassId,
    _In_opt_        PFNCREATEMINIPORT                       MiniportCreate,
    _In_            ULONG                                   cPropertyCount,
    _In_reads_opt_(cPropertyCount) const AUDIO_DEVPROPERTY * pProperties,
    _In_opt_        PVOID                                   DeviceContext,
    _In_            PENDPOINT_MINIPAIR                      MiniportPair,
    _In_opt_        PRESOURCELIST                           ResourceList,
    _In_            REFGUID                                 PortInterfaceId,
    _Out_opt_       PUNKNOWN                              * OutPortInterface,
    _Out_opt_       PUNKNOWN                              * OutPortUnknown,
    _Out_opt_       PUNKNOWN                              * OutMiniportUnknown
)
{
    PAGED_CODE();
    DPF_ENTER(("[InstallSubDevice %S]", Name));

    ASSERT(Name != NULL);
    ASSERT(m_pDeviceObject != NULL);

    NTSTATUS                    ntStatus;
    PPORT                       port            = NULL;
    PUNKNOWN                    miniport        = NULL;
    PADAPTERCOMMON              adapterCommon   = NULL;
    UNICODE_STRING              symbolicLink    = { 0 };

    adapterCommon = PADAPTERCOMMON(this);

    ntStatus = CreateAudioInterfaceWithProperties(Name, TemplateName, cPropertyCount, pProperties, &symbolicLink);
    if (NT_SUCCESS(ntStatus))
    {
        // Currently have no use for the symbolic link
        RtlFreeUnicodeString(&symbolicLink);

        ntStatus = PcNewPort(&port, PortClassId);
    }

    if (NT_SUCCESS(ntStatus))
    {
        if (MiniportCreate)
        {
            ntStatus =
                MiniportCreate
                (
                    &miniport,
                    MiniportClassId,
                    NULL,
                    POOL_FLAG_NON_PAGED,
                    adapterCommon,
                    DeviceContext,
                    MiniportPair
                );
        }
        else
        {
            ntStatus =
                PcNewMiniport
                (
                    (PMINIPORT *) &miniport,
                    MiniportClassId
                );
        }
    }

    // Init the port driver and miniport in one go.
    if (NT_SUCCESS(ntStatus))
    {
#pragma warning(push)
        // IPort::Init's annotation on ResourceList requires it to be non-NULL. However,
        // for dynamic devices, we may no longer have the resource list and this should
        // still succeed.
#pragma warning(disable:6387)
        ntStatus =
            port->Init
            (
                m_pDeviceObject,
                Irp,
                miniport,
                adapterCommon,
                ResourceList
            );
#pragma warning (pop)

        if (NT_SUCCESS(ntStatus))
        {
            ntStatus =
                PcRegisterSubdevice
                (
                    m_pDeviceObject,
                    Name,
                    port
                );
        }
    }

    // Deposit the port interfaces if needed.
    if (NT_SUCCESS(ntStatus))
    {
        if (OutPortUnknown)
        {
            ntStatus =
                port->QueryInterface
                (
                    IID_IUnknown,
                    (PVOID *)OutPortUnknown
                );
        }

        if (OutPortInterface)
        {
            ntStatus =
                port->QueryInterface
                (
                    PortInterfaceId,
                    (PVOID *) OutPortInterface
                );
        }

        if (OutMiniportUnknown)
        {
            ntStatus =
                miniport->QueryInterface
                (
                    IID_IUnknown,
                    (PVOID *)OutMiniportUnknown
                );
        }

    }

    if (port)
    {
        port->Release();
    }

    if (miniport)
    {
        miniport->Release();
    }

    return ntStatus;
} // InstallSubDevice

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CAdapterCommon::UnregisterSubdevice
(
    _In_opt_   PUNKNOWN     UnknownPort
)
{
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::UnregisterSubdevice]"));

    ASSERT(m_pDeviceObject != NULL);

    NTSTATUS                ntStatus            = STATUS_SUCCESS;
    PUNREGISTERSUBDEVICE    unregisterSubdevice = NULL;

    if (NULL == UnknownPort)
    {
        return ntStatus;
    }

    ntStatus = UnknownPort->QueryInterface(
        IID_IUnregisterSubdevice,
        (PVOID *)&unregisterSubdevice);

    if (NT_SUCCESS(ntStatus))
    {
        ntStatus = unregisterSubdevice->UnregisterSubdevice(
            m_pDeviceObject,
            UnknownPort);

        unregisterSubdevice->Release();
    }

    return ntStatus;
}

// Connects the bridge pins between the wave and topology filters for an endpoint.
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CAdapterCommon::ConnectTopologies
(
    _In_ PUNKNOWN                   UnknownTopology,
    _In_ PUNKNOWN                   UnknownWave,
    _In_ PHYSICALCONNECTIONTABLE*   PhysicalConnections,
    _In_ ULONG                      PhysicalConnectionCount
)
{
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::ConnectTopologies]"));

    ASSERT(m_pDeviceObject != NULL);

    NTSTATUS        ntStatus            = STATUS_SUCCESS;

    for (ULONG i = 0; i < PhysicalConnectionCount && NT_SUCCESS(ntStatus); i++)
    {

        switch(PhysicalConnections[i].eType)
        {
            case CONNECTIONTYPE_TOPOLOGY_OUTPUT:
                ntStatus =
                    PcRegisterPhysicalConnection
                    (
                        m_pDeviceObject,
                        UnknownTopology,
                        PhysicalConnections[i].ulTopology,
                        UnknownWave,
                        PhysicalConnections[i].ulWave
                    );
                if (!NT_SUCCESS(ntStatus))
                {
                    DPF(D_TERSE, ("ConnectTopologies: PcRegisterPhysicalConnection(render) failed, 0x%x", ntStatus));
                }
                break;
            case CONNECTIONTYPE_WAVE_OUTPUT:
                ntStatus =
                    PcRegisterPhysicalConnection
                    (
                        m_pDeviceObject,
                        UnknownWave,
                        PhysicalConnections[i].ulWave,
                        UnknownTopology,
                        PhysicalConnections[i].ulTopology
                    );
                if (!NT_SUCCESS(ntStatus))
                {
                    DPF(D_TERSE, ("ConnectTopologies: PcRegisterPhysicalConnection(capture) failed, 0x%x", ntStatus));
                }
                break;
        }
    }

    if (!NT_SUCCESS(ntStatus))
    {
        // Disconnect everything on error - ignore the error code since not all
        // connections may have been made.
        DisconnectTopologies(UnknownTopology, UnknownWave, PhysicalConnections, PhysicalConnectionCount);
    }

    return ntStatus;
}

// Disconnects the bridge pins between the wave and topology filters for an endpoint.
#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CAdapterCommon::DisconnectTopologies
(
    _In_ PUNKNOWN                   UnknownTopology,
    _In_ PUNKNOWN                   UnknownWave,
    _In_ PHYSICALCONNECTIONTABLE*   PhysicalConnections,
    _In_ ULONG                      PhysicalConnectionCount
)
{
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::DisconnectTopologies]"));

    ASSERT(m_pDeviceObject != NULL);

    NTSTATUS                        ntStatus                        = STATUS_SUCCESS;
    NTSTATUS                        ntStatus2                       = STATUS_SUCCESS;
    PUNREGISTERPHYSICALCONNECTION   unregisterPhysicalConnection    = NULL;

    ntStatus = UnknownTopology->QueryInterface(
        IID_IUnregisterPhysicalConnection,
        (PVOID *)&unregisterPhysicalConnection);

    if (NT_SUCCESS(ntStatus))
    {
        for (ULONG i = 0; i < PhysicalConnectionCount; i++)
        {
            switch(PhysicalConnections[i].eType)
            {
                case CONNECTIONTYPE_TOPOLOGY_OUTPUT:
                    ntStatus =
                        unregisterPhysicalConnection->UnregisterPhysicalConnection(
                            m_pDeviceObject,
                            UnknownTopology,
                            PhysicalConnections[i].ulTopology,
                            UnknownWave,
                            PhysicalConnections[i].ulWave
                        );

                    if (!NT_SUCCESS(ntStatus))
                    {
                        DPF(D_TERSE, ("DisconnectTopologies: UnregisterPhysicalConnection(render) failed, 0x%x", ntStatus));
                    }
                    break;
                case CONNECTIONTYPE_WAVE_OUTPUT:
                    ntStatus =
                        unregisterPhysicalConnection->UnregisterPhysicalConnection(
                            m_pDeviceObject,
                            UnknownWave,
                            PhysicalConnections[i].ulWave,
                            UnknownTopology,
                            PhysicalConnections[i].ulTopology
                        );
                    if (!NT_SUCCESS(ntStatus2))
                    {
                        DPF(D_TERSE, ("DisconnectTopologies: UnregisterPhysicalConnection(capture) failed, 0x%x", ntStatus2));
                    }
                    break;
            }

            // Cache and return the first error encountered, since it's likely the most relevant.
            if (NT_SUCCESS(ntStatus))
            {
                ntStatus = ntStatus2;
            }
        }
    }

    SAFE_RELEASE(unregisterPhysicalConnection);

    return ntStatus;
}

#pragma code_seg("PAGE")
NTSTATUS
CAdapterCommon::GetCachedSubdevice
(
    _In_ PWSTR Name,
    _Out_opt_ PUNKNOWN *OutUnknownPort,
    _Out_opt_ PUNKNOWN *OutUnknownMiniport
)
{
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::GetCachedSubdevice]"));

    // Search list, return interface to device if found, fail if not found.
    PLIST_ENTRY le = NULL;
    BOOL bFound = FALSE;

    for (le = m_SubdeviceCache.Flink; le != &m_SubdeviceCache && !bFound; le = le->Flink)
    {
        MINIPAIR_UNKNOWN *pRecord = CONTAINING_RECORD(le, MINIPAIR_UNKNOWN, ListEntry);

        if (0 == wcscmp(Name, pRecord->Name))
        {
            if (OutUnknownPort)
            {
                *OutUnknownPort = pRecord->PortInterface;
                (*OutUnknownPort)->AddRef();
            }

            if (OutUnknownMiniport)
            {
                *OutUnknownMiniport = pRecord->MiniportInterface;
                (*OutUnknownMiniport)->AddRef();
            }

            bFound = TRUE;
        }
    }

    return bFound?STATUS_SUCCESS:STATUS_OBJECT_NAME_NOT_FOUND;
}

#pragma code_seg("PAGE")
NTSTATUS
CAdapterCommon::CacheSubdevice
(
    _In_ PWSTR Name,
    _In_ PUNKNOWN UnknownPort,
    _In_ PUNKNOWN UnknownMiniport
)
{
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::CacheSubdevice]"));

    NTSTATUS         ntStatus       = STATUS_SUCCESS;
    MINIPAIR_UNKNOWN *pNewSubdevice = NULL;

    pNewSubdevice = new(POOL_FLAG_NON_PAGED, MINADAPTER_POOLTAG) MINIPAIR_UNKNOWN;

    if (!pNewSubdevice)
    {
        DPF(D_TERSE, ("Insufficient memory to cache subdevice"));
        ntStatus = STATUS_INSUFFICIENT_RESOURCES;
    }

    if (NT_SUCCESS(ntStatus))
    {
        memset(pNewSubdevice, 0, sizeof(MINIPAIR_UNKNOWN));

        ntStatus = RtlStringCchCopyW(pNewSubdevice->Name, SIZEOF_ARRAY(pNewSubdevice->Name), Name);
    }

    if (NT_SUCCESS(ntStatus))
    {
        pNewSubdevice->PortInterface = UnknownPort;
        pNewSubdevice->PortInterface->AddRef();

        pNewSubdevice->MiniportInterface = UnknownMiniport;
        pNewSubdevice->MiniportInterface->AddRef();

        // Cache the IAdapterPowerManagement interface (if available) from the filter. Some
        // endpoints, like FM and cellular, have their own power requirements to track; if
        // this fails, it just means this filter doesn't do power management.
        UnknownMiniport->QueryInterface(IID_IAdapterPowerManagement, (PVOID *)&(pNewSubdevice->PowerInterface));

        InsertTailList(&m_SubdeviceCache, &pNewSubdevice->ListEntry);
    }

    if (!NT_SUCCESS(ntStatus))
    {
        if (pNewSubdevice)
        {
            delete pNewSubdevice;
        }
    }

    return ntStatus;
}

#pragma code_seg("PAGE")
NTSTATUS
CAdapterCommon::RemoveCachedSubdevice
(
    _In_ PWSTR Name
)
{
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::RemoveCachedSubdevice]"));

    PLIST_ENTRY le = NULL;
    BOOL bRemoved = FALSE;

    for (le = m_SubdeviceCache.Flink; le != &m_SubdeviceCache && !bRemoved; le = le->Flink)
    {
        MINIPAIR_UNKNOWN *pRecord = CONTAINING_RECORD(le, MINIPAIR_UNKNOWN, ListEntry);

        if (0 == wcscmp(Name, pRecord->Name))
        {
            SAFE_RELEASE(pRecord->PortInterface);
            SAFE_RELEASE(pRecord->MiniportInterface);
            SAFE_RELEASE(pRecord->PowerInterface);
            memset(pRecord->Name, 0, sizeof(pRecord->Name));
            RemoveEntryList(le);
            bRemoved = TRUE;
            delete pRecord;
            break;
        }
    }

    return bRemoved?STATUS_SUCCESS:STATUS_OBJECT_NAME_NOT_FOUND;
}

#pragma code_seg("PAGE")
VOID
CAdapterCommon::EmptySubdeviceCache()
{
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::EmptySubdeviceCache]"));

    while (!IsListEmpty(&m_SubdeviceCache))
    {
        PLIST_ENTRY le = RemoveHeadList(&m_SubdeviceCache);
        MINIPAIR_UNKNOWN *pRecord = CONTAINING_RECORD(le, MINIPAIR_UNKNOWN, ListEntry);

        SAFE_RELEASE(pRecord->PortInterface);
        SAFE_RELEASE(pRecord->MiniportInterface);
        SAFE_RELEASE(pRecord->PowerInterface);
        memset(pRecord->Name, 0, sizeof(pRecord->Name));

        delete pRecord;
    }
}

#pragma code_seg("PAGE")
VOID
CAdapterCommon::Cleanup()
{
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::Cleanup]"));
    EmptySubdeviceCache();
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CAdapterCommon::InstallEndpointFilters
(
    _In_opt_    PIRP                Irp,
    _In_        PENDPOINT_MINIPAIR  MiniportPair,
    _In_opt_    PVOID               DeviceContext,
    _Out_opt_   PUNKNOWN *          UnknownTopology,
    _Out_opt_   PUNKNOWN *          UnknownWave,
    _Out_opt_   PUNKNOWN *          UnknownMiniportTopology,
    _Out_opt_   PUNKNOWN *          UnknownMiniportWave
)
{
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::InstallEndpointFilters]"));

    NTSTATUS            ntStatus            = STATUS_SUCCESS;
    PUNKNOWN            unknownTopology     = NULL;
    PUNKNOWN            unknownWave         = NULL;
    BOOL                bTopologyCreated    = FALSE;
    BOOL                bWaveCreated        = FALSE;
    PUNKNOWN            unknownMiniTopo     = NULL;
    PUNKNOWN            unknownMiniWave     = NULL;

    if (UnknownTopology)
    {
        *UnknownTopology = NULL;
    }

    if (UnknownWave)
    {
        *UnknownWave = NULL;
    }

    if (UnknownMiniportTopology)
    {
        *UnknownMiniportTopology = NULL;
    }

    if (UnknownMiniportWave)
    {
        *UnknownMiniportWave = NULL;
    }

    ntStatus = GetCachedSubdevice(MiniportPair->TopoName, &unknownTopology, &unknownMiniTopo);
    if (!NT_SUCCESS(ntStatus) || NULL == unknownTopology || NULL == unknownMiniTopo)
    {
        bTopologyCreated = TRUE;

        ntStatus = InstallSubdevice(Irp,
                                    MiniportPair->TopoName, // make sure this name matches with XELPH_VMS.<TopoName>.szPname in the inf's [Strings] section
                                    MiniportPair->TemplateTopoName,
                                    CLSID_PortTopology,
                                    CLSID_PortTopology,
                                    MiniportPair->TopoCreateCallback,
                                    MiniportPair->TopoInterfacePropertyCount,
                                    MiniportPair->TopoInterfaceProperties,
                                    DeviceContext,
                                    MiniportPair,
                                    NULL,
                                    IID_IPortTopology,
                                    NULL,
                                    &unknownTopology,
                                    &unknownMiniTopo
                                    );
        if (NT_SUCCESS(ntStatus))
        {
            ntStatus = CacheSubdevice(MiniportPair->TopoName, unknownTopology, unknownMiniTopo);
        }
    }

    ntStatus = GetCachedSubdevice(MiniportPair->WaveName, &unknownWave, &unknownMiniWave);
    if (!NT_SUCCESS(ntStatus) || NULL == unknownWave || NULL == unknownMiniWave)
    {
        bWaveCreated = TRUE;

        ntStatus = InstallSubdevice(Irp,
                                    MiniportPair->WaveName, // make sure this name matches with XELPH_VMS.<WaveName>.szPname in the inf's [Strings] section
                                    MiniportPair->TemplateWaveName,
                                    CLSID_PortWaveRT,
                                    CLSID_PortWaveRT,
                                    MiniportPair->WaveCreateCallback,
                                    MiniportPair->WaveInterfacePropertyCount,
                                    MiniportPair->WaveInterfaceProperties,
                                    DeviceContext,
                                    MiniportPair,
                                    NULL,
                                    IID_IPortWaveRT,
                                    NULL,
                                    &unknownWave,
                                    &unknownMiniWave
                                    );

        if (NT_SUCCESS(ntStatus))
        {
            ntStatus = CacheSubdevice(MiniportPair->WaveName, unknownWave, unknownMiniWave);
        }
    }

    if (unknownTopology && unknownWave)
    {
        ntStatus = ConnectTopologies(
            unknownTopology,
            unknownWave,
            MiniportPair->PhysicalConnections,
            MiniportPair->PhysicalConnectionCount);
    }

    if (NT_SUCCESS(ntStatus))
    {
        if (UnknownTopology != NULL && unknownTopology != NULL)
        {
            unknownTopology->AddRef();
            *UnknownTopology = unknownTopology;
        }

        if (UnknownWave != NULL && unknownWave != NULL)
        {
            unknownWave->AddRef();
            *UnknownWave = unknownWave;
        }
        if (UnknownMiniportTopology != NULL && unknownMiniTopo != NULL)
        {
            unknownMiniTopo->AddRef();
            *UnknownMiniportTopology = unknownMiniTopo;
        }

        if (UnknownMiniportWave != NULL && unknownMiniWave != NULL)
        {
            unknownMiniWave->AddRef();
            *UnknownMiniportWave = unknownMiniWave;
        }

    }
    else
    {
        if (bTopologyCreated && unknownTopology != NULL)
        {
            UnregisterSubdevice(unknownTopology);
            RemoveCachedSubdevice(MiniportPair->TopoName);
        }

        if (bWaveCreated && unknownWave != NULL)
        {
            UnregisterSubdevice(unknownWave);
            RemoveCachedSubdevice(MiniportPair->WaveName);
        }
    }

    SAFE_RELEASE(unknownMiniTopo);
    SAFE_RELEASE(unknownTopology);
    SAFE_RELEASE(unknownMiniWave);
    SAFE_RELEASE(unknownWave);

    return ntStatus;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CAdapterCommon::RemoveEndpointFilters
(
    _In_        PENDPOINT_MINIPAIR  MiniportPair,
    _In_opt_    PUNKNOWN            UnknownTopology,
    _In_opt_    PUNKNOWN            UnknownWave
)
{
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::RemoveEndpointFilters]"));

    NTSTATUS    ntStatus   = STATUS_SUCCESS;

    if (UnknownTopology != NULL && UnknownWave != NULL)
    {
        ntStatus = DisconnectTopologies(
            UnknownTopology,
            UnknownWave,
            MiniportPair->PhysicalConnections,
            MiniportPair->PhysicalConnectionCount);

        if (!NT_SUCCESS(ntStatus))
        {
            DPF(D_VERBOSE, ("RemoveEndpointFilters: DisconnectTopologies failed: 0x%x", ntStatus));
        }
    }


    RemoveCachedSubdevice(MiniportPair->WaveName);

    ntStatus = UnregisterSubdevice(UnknownWave);
    if (!NT_SUCCESS(ntStatus))
    {
        DPF(D_VERBOSE, ("RemoveEndpointFilters: UnregisterSubdevice(wave) failed: 0x%x", ntStatus));
    }

    RemoveCachedSubdevice(MiniportPair->TopoName);

    ntStatus = UnregisterSubdevice(UnknownTopology);
    if (!NT_SUCCESS(ntStatus))
    {
        DPF(D_VERBOSE, ("RemoveEndpointFilters: UnregisterSubdevice(topology) failed: 0x%x", ntStatus));
    }

    ntStatus = STATUS_SUCCESS;

    return ntStatus;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CAdapterCommon::GetFilters
(
    _In_        PENDPOINT_MINIPAIR  MiniportPair,
    _Out_opt_   PUNKNOWN *          UnknownTopologyPort,
    _Out_opt_   PUNKNOWN *          UnknownTopologyMiniport,
    _Out_opt_   PUNKNOWN *          UnknownWavePort,
    _Out_opt_   PUNKNOWN *          UnknownWaveMiniport
)
{
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::GetFilters]"));

    NTSTATUS    ntStatus   = STATUS_SUCCESS;
    PUNKNOWN            unknownTopologyPort     = NULL;
    PUNKNOWN            unknownTopologyMiniport = NULL;
    PUNKNOWN            unknownWavePort         = NULL;
    PUNKNOWN            unknownWaveMiniport     = NULL;

    // If the client requested the topology filter, find it and return it.
    if (UnknownTopologyPort != NULL || UnknownTopologyMiniport != NULL)
    {
        ntStatus = GetCachedSubdevice(MiniportPair->TopoName, &unknownTopologyPort, &unknownTopologyMiniport);
        if (NT_SUCCESS(ntStatus))
        {
            if (UnknownTopologyPort)
            {
                *UnknownTopologyPort = unknownTopologyPort;
            }

            if (UnknownTopologyMiniport)
            {
                *UnknownTopologyMiniport = unknownTopologyMiniport;
            }
        }
    }

    // If the client requested the wave filter, find it and return it.
    if (NT_SUCCESS(ntStatus) && (UnknownWavePort != NULL || UnknownWaveMiniport != NULL))
    {
        ntStatus = GetCachedSubdevice(MiniportPair->WaveName, &unknownWavePort, &unknownWaveMiniport);
        if (NT_SUCCESS(ntStatus))
        {
            if (UnknownWavePort)
            {
                *UnknownWavePort = unknownWavePort;
            }

            if (UnknownWaveMiniport)
            {
                *UnknownWaveMiniport = unknownWaveMiniport;
            }
        }
    }

    return ntStatus;
}

#pragma code_seg("PAGE")
STDMETHODIMP_(NTSTATUS)
CAdapterCommon::SetIdlePowerManagement
(
  _In_  PENDPOINT_MINIPAIR  MiniportPair,
  _In_  BOOL bEnabled
)
{
    PAGED_CODE();
    DPF_ENTER(("[CAdapterCommon::SetIdlePowerManagement]"));

    NTSTATUS      ntStatus   = STATUS_SUCCESS;
    IUnknown      *pUnknown = NULL;
    PPORTCLSPOWER pPortClsPower = NULL;
    // Refcounts disable requests. Each miniport must call this in pairs: disable
    // on the first request to disable, enable on the last request to enable.

    // Always call SetIdlePowerManagement using the IPortClsPower from the
    // requesting port, so we don't cache a reference to a port indefinitely
    // and prevent it from ever unloading.
    ntStatus = GetFilters(MiniportPair, NULL, NULL, &pUnknown, NULL);
    if (NT_SUCCESS(ntStatus))
    {
        ntStatus =
            pUnknown->QueryInterface
            (
                IID_IPortClsPower,
                (PVOID*) &pPortClsPower
            );
    }

    if (NT_SUCCESS(ntStatus))
    {
        if (bEnabled)
        {
            m_dwIdleRequests--;

            if (0 == m_dwIdleRequests)
            {
                pPortClsPower->SetIdlePowerManagement(m_pDeviceObject, TRUE);
            }
        }
        else
        {
            if (0 == m_dwIdleRequests)
            {
                pPortClsPower->SetIdlePowerManagement(m_pDeviceObject, FALSE);
            }

            m_dwIdleRequests++;
        }
    }

    SAFE_RELEASE(pUnknown);
    SAFE_RELEASE(pPortClsPower);

    return ntStatus;
}

// Copies the registry values in _hSourceKey to _hDestinationKey.
#pragma code_seg("PAGE")
NTSTATUS
CopyRegistryValues(HANDLE _hSourceKey, HANDLE _hDestinationKey)
{
    NTSTATUS                    ntStatus = STATUS_SUCCESS;
    PKEY_VALUE_FULL_INFORMATION kvFullInfo = NULL;
    ULONG                       ulFullInfoLength = 0;
    ULONG                       ulFullInfoResultLength = 0;
    PWSTR                       pwstrKeyValueName = NULL;
    UNICODE_STRING              strKeyValueName;
    PAGED_CODE();
    // Allocate the KEY_VALUE_FULL_INFORMATION structure
    ulFullInfoLength = sizeof(KEY_VALUE_FULL_INFORMATION) + MAX_DEVICE_REG_KEY_LENGTH;
    kvFullInfo = (PKEY_VALUE_FULL_INFORMATION)ExAllocatePool2(POOL_FLAG_NON_PAGED, ulFullInfoLength, MINADAPTER_POOLTAG);
    IF_TRUE_ACTION_JUMP(kvFullInfo == NULL, ntStatus = STATUS_INSUFFICIENT_RESOURCES, Exit);

    // Iterate over each value and copy it to the destination
    for (UINT i = 0; NT_SUCCESS(ntStatus); i++)
    {
        // Enumerate the next value
        ntStatus = ZwEnumerateValueKey(_hSourceKey, i, KeyValueFullInformation, kvFullInfo, ulFullInfoLength, &ulFullInfoResultLength);

        // Jump out of this loop if there are no more values
        IF_TRUE_ACTION_JUMP(ntStatus == STATUS_NO_MORE_ENTRIES, ntStatus = STATUS_SUCCESS, Exit);

        // Handle incorrect buffer size
        if (ntStatus == STATUS_BUFFER_TOO_SMALL || ntStatus == STATUS_BUFFER_OVERFLOW)
        {
            // Free and re-allocate the KEY_VALUE_FULL_INFORMATION structure with the correct size
            ExFreePoolWithTag(kvFullInfo, MINADAPTER_POOLTAG);

            ulFullInfoLength = ulFullInfoResultLength;

            kvFullInfo = (PKEY_VALUE_FULL_INFORMATION)ExAllocatePool2(POOL_FLAG_NON_PAGED, ulFullInfoLength, MINADAPTER_POOLTAG);
            IF_TRUE_ACTION_JUMP(kvFullInfo == NULL, ntStatus = STATUS_INSUFFICIENT_RESOURCES, loop_exit);

            // Try to enumerate the current value again
            ntStatus = ZwEnumerateValueKey(_hSourceKey, i, KeyValueFullInformation, kvFullInfo, ulFullInfoLength, &ulFullInfoResultLength);

            // Jump out of this loop if there are no more values
            IF_TRUE_ACTION_JUMP(ntStatus == STATUS_NO_MORE_ENTRIES, ntStatus = STATUS_SUCCESS, Exit);
            IF_FAILED_JUMP(ntStatus, loop_exit);
        }
        else
        {
            IF_FAILED_JUMP(ntStatus, loop_exit);
        }

        // Allocate the key value name string
        pwstrKeyValueName = (PWSTR)ExAllocatePool2(POOL_FLAG_NON_PAGED, kvFullInfo->NameLength + sizeof(WCHAR)*2, MINADAPTER_POOLTAG);
        IF_TRUE_ACTION_JUMP(pwstrKeyValueName == NULL, ntStatus = STATUS_INSUFFICIENT_RESOURCES, loop_exit);

        // Copy the key value name from the full information struct
        RtlStringCbCopyNW(pwstrKeyValueName, kvFullInfo->NameLength + sizeof(WCHAR)*2, kvFullInfo->Name, kvFullInfo->NameLength);

        // Make sure the string is null terminated
        pwstrKeyValueName[(kvFullInfo->NameLength) / sizeof(WCHAR)] = 0;

        // Copy the key value name string to a UNICODE string
        RtlInitUnicodeString(&strKeyValueName, pwstrKeyValueName);

        // Write the key value from the source into the destination
        ntStatus = ZwSetValueKey(_hDestinationKey, &strKeyValueName, 0, kvFullInfo->Type, (PVOID)((PUCHAR)kvFullInfo + kvFullInfo->DataOffset), kvFullInfo->DataLength);
        IF_FAILED_JUMP(ntStatus, loop_exit);

    loop_exit:
        // Free the key value name string
        if (pwstrKeyValueName)
        {
            ExFreePoolWithTag(pwstrKeyValueName, MINADAPTER_POOLTAG);
            pwstrKeyValueName = NULL;
        }

        // Bail if anything failed
        IF_FAILED_JUMP(ntStatus, Exit);
    }

Exit:
    // Free the KEY_VALUE_FULL_INFORMATION structure
    if (kvFullInfo)
    {
        ExFreePoolWithTag(kvFullInfo, MINADAPTER_POOLTAG);
    }

    return ntStatus;
}

// Recursively copies the registry values/subkeys of _hSourceKey into
// _hDestinationKey. _bOverwrite controls whether the top-level values are
// copied: pass FALSE on the initial call (so friendly name/PNP properties
// aren't touched) - every recursive call on subkeys passes TRUE, so
// everything below the first level is copied in full.
#pragma code_seg("PAGE")
NTSTATUS
CopyRegistryKey(HANDLE _hSourceKey, HANDLE _hDestinationKey, BOOL _bOverwrite = FALSE)
{
    NTSTATUS                ntStatus = STATUS_UNSUCCESSFUL;
    PKEY_BASIC_INFORMATION  kBasicInfo = NULL;
    ULONG                   ulBasicInfoLength = 0;
    ULONG                   ulBasicInfoResultLength = 0;
    ULONG                   ulDisposition = 0;
    PWSTR                   pwstrKeyName = NULL;
    UNICODE_STRING          strKeyName;
    OBJECT_ATTRIBUTES       hCurrentSourceKeyAttributes;
    OBJECT_ATTRIBUTES       hNewDestinationKeyAttributes;
    HANDLE                  hCurrentSourceKey = NULL;
    HANDLE                  hNewDestinationKey = NULL;
    PAGED_CODE();
    // Validate parameters
    IF_TRUE_ACTION_JUMP(_hSourceKey == nullptr, ntStatus = STATUS_INVALID_PARAMETER, Exit);
    IF_TRUE_ACTION_JUMP(_hDestinationKey == nullptr, ntStatus = STATUS_INVALID_PARAMETER, Exit);

    // Allocate the KEY_BASIC_INFORMATION structure
    ulBasicInfoLength = sizeof(KEY_BASIC_INFORMATION) + MAX_DEVICE_REG_KEY_LENGTH;
    kBasicInfo = (PKEY_BASIC_INFORMATION)ExAllocatePool2(POOL_FLAG_NON_PAGED, ulBasicInfoLength, MINADAPTER_POOLTAG);
    IF_TRUE_ACTION_JUMP(kBasicInfo == NULL, ntStatus = STATUS_INSUFFICIENT_RESOURCES, Exit);

    ntStatus = STATUS_SUCCESS;
    // Iterate over each key and copy it
    for (UINT i = 0; NT_SUCCESS(ntStatus); i++)
    {
        // Enumerate the next key
        ntStatus = ZwEnumerateKey(_hSourceKey, i, KeyBasicInformation, kBasicInfo, ulBasicInfoLength, &ulBasicInfoResultLength);

        // Jump out of this loop if there are no more keys
        IF_TRUE_ACTION_JUMP(ntStatus == STATUS_NO_MORE_ENTRIES, ntStatus = STATUS_SUCCESS, copy_values);

        // Handle incorrect buffer size
        if (ntStatus == STATUS_BUFFER_TOO_SMALL || ntStatus == STATUS_BUFFER_OVERFLOW)
        {
            // Free and re-allocate the KEY_BASIC_INFORMATION structure with the correct size.
            ExFreePoolWithTag(kBasicInfo, MINADAPTER_POOLTAG);
            ulBasicInfoLength = ulBasicInfoResultLength;
            kBasicInfo = (PKEY_BASIC_INFORMATION)ExAllocatePool2(POOL_FLAG_NON_PAGED, ulBasicInfoLength, MINADAPTER_POOLTAG);
            IF_TRUE_ACTION_JUMP(kBasicInfo == NULL, ntStatus = STATUS_INSUFFICIENT_RESOURCES, loop_exit);

            // Try to enumerate the current key again.
            ntStatus = ZwEnumerateKey(_hSourceKey, i, KeyBasicInformation, kBasicInfo, ulBasicInfoLength, &ulBasicInfoResultLength);

            // Jump out of this loop if there are no more keys
            IF_TRUE_ACTION_JUMP(ntStatus == STATUS_NO_MORE_ENTRIES, ntStatus = STATUS_SUCCESS, copy_values);
            IF_FAILED_JUMP(ntStatus, loop_exit);
        }
        else
        {
            IF_FAILED_JUMP(ntStatus, loop_exit);
        }

        // Allocate the key name string
        pwstrKeyName = (PWSTR)ExAllocatePool2(POOL_FLAG_NON_PAGED, kBasicInfo->NameLength + sizeof(WCHAR), MINADAPTER_POOLTAG);
        IF_TRUE_ACTION_JUMP(pwstrKeyName == NULL, ntStatus = STATUS_INSUFFICIENT_RESOURCES, loop_exit);

        // Copy the key name from the basic information struct
        RtlStringCbCopyNW(pwstrKeyName, kBasicInfo->NameLength + sizeof(WCHAR), kBasicInfo->Name, kBasicInfo->NameLength);

        // Make sure the string is null terminated
        pwstrKeyName[(kBasicInfo->NameLength) / sizeof(WCHAR)] = 0;

        // Copy the key name string to a UNICODE string
        RtlInitUnicodeString(&strKeyName, pwstrKeyName);

        // Initialize attributes to open the currently enumerated source key
        InitializeObjectAttributes(&hCurrentSourceKeyAttributes, &strKeyName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, _hSourceKey, NULL);

        // Open the currently enumerated source key
        ntStatus = ZwOpenKey(&hCurrentSourceKey, KEY_READ, &hCurrentSourceKeyAttributes);
        IF_FAILED_ACTION_JUMP(ntStatus, ZwClose(hCurrentSourceKey), loop_exit);

        // Initialize attributes to create the new destination key
        InitializeObjectAttributes(&hNewDestinationKeyAttributes, &strKeyName, OBJ_KERNEL_HANDLE, _hDestinationKey, NULL);

        // Create the key at the destination
        ntStatus = ZwCreateKey(&hNewDestinationKey, KEY_WRITE, &hNewDestinationKeyAttributes, 0, NULL, REG_OPTION_NON_VOLATILE, &ulDisposition);
        IF_FAILED_ACTION_JUMP(ntStatus, ZwClose(hCurrentSourceKey), loop_exit);

        // Now copy the contents of the currently enumerated key to the destination
        ntStatus = CopyRegistryKey(hCurrentSourceKey, hNewDestinationKey, TRUE);
        IF_FAILED_JUMP(ntStatus, loop_exit);

    loop_exit:
        // Free the key name string
        if (pwstrKeyName)
        {
            ExFreePoolWithTag(pwstrKeyName, MINADAPTER_POOLTAG);
            pwstrKeyName = NULL;
        }

        // Close the current source key
        if (hCurrentSourceKey)
        {
            ZwClose(hCurrentSourceKey);
        }

        // Close the new destination key
        if (hNewDestinationKey)
        {
            ZwClose(hNewDestinationKey);
        }

        // Bail if anything failed
        IF_FAILED_JUMP(ntStatus, Exit);
    }

copy_values:
    // Copy the values
    if (_bOverwrite)
    {
        ntStatus = CopyRegistryValues(_hSourceKey, _hDestinationKey);
        IF_FAILED_JUMP(ntStatus, Exit);
    }

Exit:
    // Free the basic information structure
    if (kBasicInfo)
    {
        ExFreePoolWithTag(kBasicInfo, MINADAPTER_POOLTAG);
    }
    return ntStatus;
}

// Copies every property from a template device interface (as declared in the
// INF) onto the real, possibly dynamically-generated, interface being
// activated. This lets one static INF entry serve multiple runtime endpoints:
// e.g. an INF interface with reference string "SpeakerWave" can be used as
// the TemplateName for a runtime-generated "SpeakerWave-1234ABCDE", and every
// parameter set on "SpeakerWave" in the INF (APOs, etc.) is copied over to
// the generated instance. Only the 2nd level of registry keys and deeper are
// copied by default, so the friendly name and other PNP properties are left
// alone while EP/FX properties are carried across.
#pragma code_seg("PAGE")
NTSTATUS CAdapterCommon::MigrateDeviceInterfaceTemplateParameters
(
    _In_ PUNICODE_STRING    SymbolicLinkName,
    _In_opt_ PCWSTR         TemplateReferenceString
)
{
    NTSTATUS            ntStatus = STATUS_SUCCESS;
    HANDLE              hDeviceInterfaceParametersKey(NULL);
    HANDLE              hTemplateDeviceInterfaceParametersKey(NULL);
    UNICODE_STRING      TemplateSymbolicLinkName;
    UNICODE_STRING      referenceString;

    PAGED_CODE();

    RtlInitUnicodeString(&TemplateSymbolicLinkName, NULL);
    RtlInitUnicodeString(&referenceString, TemplateReferenceString);

    // Register an audio interface if not already present for the template interface, so we can
    // access the registry path. If it's already registered, this simply returns the symbolic
    // link name; there's no mechanism to unregister it, and it's never made active.
    ntStatus = IoRegisterDeviceInterface(
        GetPhysicalDeviceObject(),
        &KSCATEGORY_AUDIO,
        &referenceString,
        &TemplateSymbolicLinkName);

    // Open the template device interface's registry key path
    ntStatus = IoOpenDeviceInterfaceRegistryKey(&TemplateSymbolicLinkName, GENERIC_READ, &hTemplateDeviceInterfaceParametersKey);
    IF_FAILED_JUMP(ntStatus, Exit);

    // Open the new device interface's registry key path that we plan to activate
    ntStatus = IoOpenDeviceInterfaceRegistryKey(SymbolicLinkName, GENERIC_WRITE, &hDeviceInterfaceParametersKey);
    IF_FAILED_JUMP(ntStatus, Exit);

    // Copy the template device parameters key to the device interface key
    ntStatus = CopyRegistryKey(hTemplateDeviceInterfaceParametersKey, hDeviceInterfaceParametersKey);
    IF_FAILED_JUMP(ntStatus, Exit);

Exit:
    RtlFreeUnicodeString(&TemplateSymbolicLinkName);

    if (hTemplateDeviceInterfaceParametersKey)
    {
        ZwClose(hTemplateDeviceInterfaceParametersKey);
    }

    if (hDeviceInterfaceParametersKey)
    {
        ZwClose(hDeviceInterfaceParametersKey);
    }

    return ntStatus;
}
