/*++

Copyright (c) Xelph. All Rights Reserved.

Module Name:

    outputendpoint.cpp

Abstract:

    PortCls property-request entry points for the speaker topology filter:
    jack description (KSPROPSETID_Jack) and generic topology properties,
    both redirected to the CMiniportTopology instance in MajorTarget.

Contact:

    Discord: xelphh
    Website: https://Xelph.lol

--*/

#pragma warning (disable : 4127)

#include "shared.h"
#include "pinmap.h"
#include "outputtopology.h"
#include "outputendpoint.h"
#include "outputtopologytables.h"

#pragma code_seg("PAGE")
NTSTATUS
PropertyHandler_SpeakerTopoFilter
(
    _In_ PPCPROPERTY_REQUEST      PropertyRequest
)
{
    PAGED_CODE();

    ASSERT(PropertyRequest);

    DPF_ENTER(("[PropertyHandler_SpeakerTopoFilter]"));

    // PropertyRequest is filled in by PortCls; MajorTarget is the miniport instance.
    NTSTATUS            ntStatus = STATUS_INVALID_DEVICE_REQUEST;
    PCMiniportTopology  pMiniport = (PCMiniportTopology)PropertyRequest->MajorTarget;

    if (IsEqualGUIDAligned(*PropertyRequest->PropertyItem->Set, KSPROPSETID_Jack))
    {
        if (PropertyRequest->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION)
        {
            ntStatus = pMiniport->PropertyHandlerJackDescription(
                PropertyRequest,
                ARRAYSIZE(SpeakerJackDescriptions),
                SpeakerJackDescriptions
                );
        }
        else if (PropertyRequest->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION2)
        {
            ntStatus = pMiniport->PropertyHandlerJackDescription2(
                PropertyRequest,
                ARRAYSIZE(SpeakerJackDescriptions),
                SpeakerJackDescriptions,
                0 // jack capabilities
                );
        }
    }

    return ntStatus;
} // PropertyHandler_SpeakerTopoFilter

NTSTATUS
PropertyHandler_SpeakerTopology
(
    _In_ PPCPROPERTY_REQUEST      PropertyRequest
)
{
    PAGED_CODE();

    ASSERT(PropertyRequest);

    DPF_ENTER(("[PropertyHandler_SpeakerTopology]"));

    PCMiniportTopology pMiniport = (PCMiniportTopology)PropertyRequest->MajorTarget;

    return pMiniport->PropertyHandlerGeneric(PropertyRequest);
} // PropertyHandler_SpeakerTopology

#pragma code_seg()
