/*++

Copyright (c) Xelph. All Rights Reserved.

Module Name:

    outputendpoint.h

Abstract:

    Declares the speaker topology filter's property handlers (see
    outputendpoint.cpp).

Contact:

    Discord: xelphh
    Website: https://Xelph.lol

--*/

#ifndef _OUTPUTENDPOINT_H_
#define _OUTPUTENDPOINT_H_

NTSTATUS PropertyHandler_SpeakerTopoFilter(_In_ PPCPROPERTY_REQUEST PropertyRequest);

NTSTATUS PropertyHandler_SpeakerTopology(_In_ PPCPROPERTY_REQUEST PropertyRequest);

#endif // _OUTPUTENDPOINT_H_
