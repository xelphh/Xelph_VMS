/*++

Copyright (c) Xelph. All Rights Reserved.

Module Name:

    looprelay.h

Abstract:

    A small ring buffer that carries rendered PCM audio from the Speaker's
    WaveRT stream over to the Microphone's WaveRT stream, so that whatever
    is played "into" this virtual speaker is heard back "from" this virtual
    microphone (similar in spirit to VB-Cable).

    All samples are carried through the ring buffer in a fixed "hub" format
    (16-bit signed, interleaved stereo, 48 kHz). Callers convert their own
    negotiated wave format to/from this hub format using the helper
    functions below.

Contact:

    Discord: xelphh
    Website: https://Xelph.lol

--*/

#ifndef _LOOPRELAY_H_
#define _LOOPRELAY_H_

#define LOOPBACK_HUB_SAMPLE_RATE       48000
#define LOOPBACK_HUB_CHANNELS          2
#define LOOPBACK_BUFFER_DURATION_MS    500
#define LOOPBACK_POOLTAG                'pbLV'

//
// Simple byte-oriented ring buffer of hub-format PCM (16-bit stereo @ 48kHz).
//
// Write() overwrites the oldest buffered data if the buffer is full (so a
// microphone that isn't currently open never causes the speaker side to
// stall or grow without bound).
//
// Read() silence-fills any shortfall if not enough data has been buffered
// yet (so the microphone reads silence, not garbage, whenever nothing is
// currently playing on the speaker).
//
// Safe to call at up to DISPATCH_LEVEL.
//
class CLoopbackRingBuffer
{
public:
    CLoopbackRingBuffer();
    ~CLoopbackRingBuffer();

    NTSTATUS Init(_In_ ULONG BufferDurationMs);

    VOID Write(_In_reads_bytes_(cbData) const BYTE* pData, _In_ ULONG cbData);
    VOID Read(_Out_writes_bytes_(cbRequested) BYTE* pBuffer, _In_ ULONG cbRequested);

    VOID Reset();

private:
    BYTE*       m_pBuffer;
    ULONG       m_cbCapacity;
    ULONG       m_ulWriteOffset;
    ULONG       m_ulValidBytes;
    KSPIN_LOCK  m_SpinLock;
};

//
// Converts FrameCount frames (always 2 channels) of an arbitrary supported
// wave format to interleaved 16-bit signed stereo (the ring buffer's hub
// format). Supports 8/16/24/32-bit integer PCM and 32-bit IEEE float.
//
VOID
LoopbackConvertToHub(
    _In_reads_bytes_((size_t)FrameCount * pSrcFormat->Format.nBlockAlign) const BYTE* pIn,
    _In_ ULONG FrameCount,
    _In_ const WAVEFORMATEXTENSIBLE* pSrcFormat,
    _Out_writes_((size_t)FrameCount * LOOPBACK_HUB_CHANNELS) SHORT* pOutHub
    );

//
// Inverse of LoopbackConvertToHub: expands hub-format stereo samples back
// out to an arbitrary supported wave format.
//
VOID
LoopbackConvertFromHub(
    _In_reads_((size_t)FrameCount * LOOPBACK_HUB_CHANNELS) const SHORT* pInHub,
    _In_ ULONG FrameCount,
    _In_ const WAVEFORMATEXTENSIBLE* pDstFormat,
    _Out_writes_bytes_((size_t)FrameCount * pDstFormat->Format.nBlockAlign) BYTE* pOut
    );

//
// Basic fixed-point linear-interpolation resampler for interleaved 16-bit
// stereo audio. Used to bridge the (common) case where the speaker and
// microphone are running at different sample rates. Each call is
// independent (no state carried across calls), which is a deliberate
// simplification for this "basic" loopback - for best quality, set both
// the virtual speaker and virtual microphone to the same sample rate/bit
// depth (e.g. 48kHz) in Windows' Sound Control Panel "Advanced" tab.
//
VOID
LoopbackResampleStereo16(
    _In_reads_((size_t)SrcFrames * LOOPBACK_HUB_CHANNELS) const SHORT* pSrc,
    _In_ ULONG SrcFrames,
    _In_ ULONG SrcRate,
    _Out_writes_((size_t)DstFrames * LOOPBACK_HUB_CHANNELS) SHORT* pDst,
    _In_ ULONG DstFrames,
    _In_ ULONG DstRate
    );

#endif // _LOOPRELAY_H_
