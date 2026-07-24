/*++

Copyright (c) Xelph. All Rights Reserved.

Module Name:

    mixerstate.cpp

Abstract:

    Implementation of CMixerState - simple array-backed getters/setters
    standing in for real mixer hardware registers (volume, mute, peak
    meter, mux selection, and the dev-specific bool/int/uint test values).

Contact:

    Discord: xelphh
    Website: https://Xelph.lol

--*/
#include "shared.h"
#include "mixerstate.h"

#pragma code_seg("PAGE")
CMixerState::CMixerState()
: m_ulMux(0),
    m_bDevSpecific(FALSE),
    m_iDevSpecific(0),
    m_uiDevSpecific(0)
{
    PAGED_CODE();

    MixerReset();
} // MixerState
#pragma code_seg()

BOOL
CMixerState::bGetDevSpecific()
{
    return m_bDevSpecific;
} // bGetDevSpecific

void
CMixerState::bSetDevSpecific
(
    _In_  BOOL                bDevSpecific
)
{
    m_bDevSpecific = bDevSpecific;
} // bSetDevSpecific

INT
CMixerState::iGetDevSpecific()
{
    return m_iDevSpecific;
} // iGetDevSpecific

void
CMixerState::iSetDevSpecific
(
    _In_  INT                 iDevSpecific
)
{
    m_iDevSpecific = iDevSpecific;
} // iSetDevSpecific

UINT
CMixerState::uiGetDevSpecific()
{
    return m_uiDevSpecific;
} // uiGetDevSpecific

void
CMixerState::uiSetDevSpecific
(
    _In_  UINT                uiDevSpecific
)
{
    m_uiDevSpecific = uiDevSpecific;
} // uiSetDevSpecific

BOOL
CMixerState::GetMixerMute
(
    _In_  ULONG                   ulNode,
    _In_  ULONG                   ulChannel
)
{
    UNREFERENCED_PARAMETER(ulChannel);

    if (ulNode < MAX_TOPOLOGY_NODES)
    {
        return m_MuteControls[ulNode];
    }

    return 0;
} // GetMixerMute

ULONG
CMixerState::GetMixerMux()
{
    return m_ulMux;
} // GetMixerMux

LONG
CMixerState::GetMixerVolume
(
    _In_  ULONG                   ulNode,
    _In_  ULONG                   ulChannel
)
{
    UNREFERENCED_PARAMETER(ulChannel);

    if (ulNode < MAX_TOPOLOGY_NODES)
    {
        return m_VolumeControls[ulNode];
    }

    return 0;
} // GetMixerVolume

LONG
CMixerState::GetMixerPeakMeter
(
    _In_  ULONG                   ulNode,
    _In_  ULONG                   ulChannel
)
{
    UNREFERENCED_PARAMETER(ulChannel);

    if (ulNode < MAX_TOPOLOGY_NODES)
    {
        return m_PeakMeterControls[ulNode];
    }

    return 0;
} // GetMixerVolume

#pragma code_seg("PAGE")
void
CMixerState::MixerReset()
{
    PAGED_CODE();

    RtlFillMemory(m_VolumeControls, sizeof(LONG) * MAX_TOPOLOGY_NODES, 0xFF);
    // Endpoints are not muted by default.
    RtlZeroMemory(m_MuteControls, sizeof(BOOL) * MAX_TOPOLOGY_NODES);

    for (ULONG i=0; i<MAX_TOPOLOGY_NODES; ++i)
    {
        m_PeakMeterControls[i] = PEAKMETER_SIGNED_MAXIMUM/2;
    }

    // BUGBUG change this depending on the topology
    m_ulMux = 2;
} // MixerReset
#pragma code_seg()

void
CMixerState::SetMixerMute
(
    _In_  ULONG                   ulNode,
    _In_  ULONG                   ulChannel,
    _In_  BOOL                    fMute
)
{
    UNREFERENCED_PARAMETER(ulChannel);

    if (ulNode < MAX_TOPOLOGY_NODES)
    {
        m_MuteControls[ulNode] = fMute;
    }
} // SetMixerMute

void
CMixerState::SetMixerMux
(
    _In_  ULONG                   ulNode
)
{
    m_ulMux = ulNode;
} // SetMixMux

void
CMixerState::SetMixerVolume
(
    _In_  ULONG                   ulNode,
    _In_  ULONG                   ulChannel,
    _In_  LONG                    lVolume
)
{
    UNREFERENCED_PARAMETER(ulChannel);

    if (ulNode < MAX_TOPOLOGY_NODES)
    {
        m_VolumeControls[ulNode] = lVolume;
    }
} // SetMixerVolume
