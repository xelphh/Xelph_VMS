/*++

Copyright (c) Xelph. All Rights Reserved.

Module Name:

    outputtopology.h

Abstract:

    Declaration of CMiniportTopology, the speaker-side topology miniport
    (see Main/outputtopology.cpp).

Contact:

    Discord: xelphh
    Website: https://Xelph.lol

--*/

#ifndef _OUTPUTTOPOLOGY_H_
#define _OUTPUTTOPOLOGY_H_

#include "topologybase.h"

class CMiniportTopology :
    public CTopologyBase,
    public IMiniportTopology,
    public CUnknown
{
  private:
    eDeviceType             m_DeviceType;

    union {
        PVOID               m_DeviceContext;
    };

public:
    DECLARE_STD_UNKNOWN();
    CMiniportTopology
    (
        _In_opt_    PUNKNOWN                UnknownOuter,
        _In_        PCFILTER_DESCRIPTOR    *FilterDesc,
        _In_        USHORT                  DeviceMaxChannels,
        _In_        eDeviceType             DeviceType, 
        _In_opt_    PVOID                   DeviceContext
    )
    : CUnknown(UnknownOuter),
      CTopologyBase(FilterDesc, DeviceMaxChannels),
      m_DeviceType(DeviceType),
      m_DeviceContext(DeviceContext)
    {
    }

    ~CMiniportTopology();

    IMP_IMiniportTopology;

    NTSTATUS PropertyHandlerJackDescription
    (
        _In_        PPCPROPERTY_REQUEST                         PropertyRequest,
        _In_        ULONG                                       cJackDescriptions,
        _In_reads_(cJackDescriptions) PKSJACK_DESCRIPTION       *JackDescriptions
    );

    NTSTATUS PropertyHandlerJackDescription2
    ( 
        _In_        PPCPROPERTY_REQUEST                         PropertyRequest,
        _In_        ULONG                                       cJackDescriptions,
        _In_reads_(cJackDescriptions) PKSJACK_DESCRIPTION       *JackDescriptions,
        _In_        DWORD                                       JackCapabilities
    );
    
    PVOID GetDeviceContext() { return m_DeviceContext;  }
};

typedef CMiniportTopology *PCMiniportTopology;

#endif // _OUTPUTTOPOLOGY_H_
