/*++

Copyright (c) Xelph. All Rights Reserved.

Module Name:

    looprelay.cpp

Abstract:

    Implementation of the speaker->microphone loopback ring buffer and its
    PCM format-conversion / resampling helpers. See looprelay.h.

Contact:

    Discord: xelphh
    Website: https://Xelph.lol

--*/
#include "shared.h"
#include "looprelay.h"

//=============================================================================
// CLoopbackRingBuffer
//=============================================================================

CLoopbackRingBuffer::CLoopbackRingBuffer()
    : m_pBuffer(NULL),
      m_cbCapacity(0),
      m_ulWriteOffset(0),
      m_ulValidBytes(0)
{
    KeInitializeSpinLock(&m_SpinLock);
}

CLoopbackRingBuffer::~CLoopbackRingBuffer()
{
    if (m_pBuffer)
    {
        ExFreePoolWithTag(m_pBuffer, LOOPBACK_POOLTAG);
        m_pBuffer = NULL;
    }
}

#pragma code_seg("PAGE")
NTSTATUS CLoopbackRingBuffer::Init(_In_ ULONG BufferDurationMs)
{
    PAGED_CODE();

    ULONG bytesPerFrame = LOOPBACK_HUB_CHANNELS * sizeof(SHORT);
    ULONG bytesPerSec   = LOOPBACK_HUB_SAMPLE_RATE * bytesPerFrame;

    m_cbCapacity = (ULONG)(((ULONGLONG)bytesPerSec * BufferDurationMs) / 1000);
    m_cbCapacity -= m_cbCapacity % bytesPerFrame;

    if (m_cbCapacity == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    m_pBuffer = (BYTE*)ExAllocatePool2(POOL_FLAG_NON_PAGED, m_cbCapacity, LOOPBACK_POOLTAG);
    if (!m_pBuffer)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(m_pBuffer, m_cbCapacity);
    m_ulWriteOffset = 0;
    m_ulValidBytes  = 0;

    return STATUS_SUCCESS;
}
#pragma code_seg()

VOID CLoopbackRingBuffer::Reset()
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_SpinLock, &oldIrql);
    m_ulWriteOffset = 0;
    m_ulValidBytes  = 0;
    KeReleaseSpinLock(&m_SpinLock, oldIrql);
}

VOID CLoopbackRingBuffer::Write(_In_reads_bytes_(cbData) const BYTE* pData, _In_ ULONG cbData)
{
    if (m_pBuffer == NULL || cbData == 0)
    {
        return;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_SpinLock, &oldIrql);

    const BYTE* pSrc      = pData;
    ULONG       cbRemaining = cbData;

    // If a single write is bigger than our whole capacity, only its tail matters.
    if (cbRemaining > m_cbCapacity)
    {
        pSrc        += (cbRemaining - m_cbCapacity);
        cbRemaining  = m_cbCapacity;
    }

    while (cbRemaining > 0)
    {
        ULONG cbChunk = min(cbRemaining, m_cbCapacity - m_ulWriteOffset);

        RtlCopyMemory(m_pBuffer + m_ulWriteOffset, pSrc, cbChunk);

        m_ulWriteOffset = (m_ulWriteOffset + cbChunk) % m_cbCapacity;
        pSrc            += cbChunk;
        cbRemaining     -= cbChunk;
    }

    // Grow the valid region, clamped to capacity (older data is implicitly
    // dropped once the reader falls that far behind).
    m_ulValidBytes = min(m_ulValidBytes + cbData, m_cbCapacity);

    KeReleaseSpinLock(&m_SpinLock, oldIrql);
}

VOID CLoopbackRingBuffer::Read(_Out_writes_bytes_(cbRequested) BYTE* pBuffer, _In_ ULONG cbRequested)
{
    if (cbRequested == 0)
    {
        return;
    }

    if (m_pBuffer == NULL)
    {
        RtlZeroMemory(pBuffer, cbRequested);
        return;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_SpinLock, &oldIrql);

    ULONG cbAvailable   = min(cbRequested, m_ulValidBytes);
    ULONG ulReadOffset  = (m_ulWriteOffset + m_cbCapacity - m_ulValidBytes) % m_cbCapacity;
    ULONG cbRemaining   = cbAvailable;
    BYTE* pDst          = pBuffer;

    while (cbRemaining > 0)
    {
        ULONG cbChunk = min(cbRemaining, m_cbCapacity - ulReadOffset);

        RtlCopyMemory(pDst, m_pBuffer + ulReadOffset, cbChunk);

        ulReadOffset  = (ulReadOffset + cbChunk) % m_cbCapacity;
        pDst          += cbChunk;
        cbRemaining   -= cbChunk;
    }

    m_ulValidBytes -= cbAvailable;

    KeReleaseSpinLock(&m_SpinLock, oldIrql);

    // Silence-fill whatever we couldn't supply (e.g. nothing is playing yet).
    if (cbAvailable < cbRequested)
    {
        RtlZeroMemory(pBuffer + cbAvailable, cbRequested - cbAvailable);
    }
}

//=============================================================================
// Format conversion helpers
//=============================================================================

//
// A format is only trusted to carry a real WAVEFORMATEXTENSIBLE (i.e. it is
// safe to read pFormat->SubFormat) when wFormatTag itself says so; a legacy
// non-extensible client format may not have allocated that much memory.
//
static __forceinline BOOLEAN
LoopbackIsFloatFormat(_In_ const WAVEFORMATEXTENSIBLE* pFormat)
{
    if (pFormat->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        return IsEqualGUIDAligned(pFormat->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) &&
               (pFormat->Format.wBitsPerSample == 32);
    }

    return pFormat->Format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
}

VOID
LoopbackConvertToHub(
    _In_reads_bytes_((size_t)FrameCount * pSrcFormat->Format.nBlockAlign) const BYTE* pIn,
    _In_ ULONG FrameCount,
    _In_ const WAVEFORMATEXTENSIBLE* pSrcFormat,
    _Out_writes_((size_t)FrameCount * LOOPBACK_HUB_CHANNELS) SHORT* pOutHub
    )
{
    WORD  bits            = pSrcFormat->Format.wBitsPerSample;
    ULONG bytesPerFrame    = pSrcFormat->Format.nBlockAlign;
    ULONG bytesPerSample   = bytesPerFrame / LOOPBACK_HUB_CHANNELS;
    BOOLEAN isFloat        = LoopbackIsFloatFormat(pSrcFormat);

    KFLOATING_SAVE floatSave;
    BOOLEAN floatSaved = FALSE;
    if (isFloat)
    {
        floatSaved = NT_SUCCESS(KeSaveFloatingPointState(&floatSave));
    }

    for (ULONG i = 0; i < FrameCount; ++i)
    {
        const BYTE* pFrame = pIn + (size_t)i * bytesPerFrame;

        for (ULONG ch = 0; ch < LOOPBACK_HUB_CHANNELS; ++ch)
        {
            const BYTE* pSample = pFrame + (size_t)ch * bytesPerSample;
            LONG value;

            if (isFloat)
            {
                if (floatSaved)
                {
                    float f = *(const float UNALIGNED*)pSample;
                    if (f > 1.0f)  f = 1.0f;
                    if (f < -1.0f) f = -1.0f;
                    value = (LONG)(f * 32767.0f);
                }
                else
                {
                    value = 0;
                }
            }
            else
            {
                switch (bits)
                {
                case 8:
                    // 8-bit PCM is unsigned, centered at 128.
                    value = ((LONG)pSample[0] - 128) * 256;
                    break;

                case 16:
                    value = *(const SHORT UNALIGNED*)pSample;
                    break;

                case 24:
                    {
                        LONG v = (LONG)((ULONG)pSample[0] | ((ULONG)pSample[1] << 8) | ((ULONG)pSample[2] << 16));
                        if (v & 0x00800000)
                        {
                            v |= (LONG)0xFF000000; // sign-extend 24 -> 32
                        }
                        value = v >> 8; // scale 24-bit range down to 16-bit
                    }
                    break;

                case 32:
                default:
                    value = (*(const LONG UNALIGNED*)pSample) >> 16; // scale 32-bit range down to 16-bit
                    break;
                }
            }

            if (value > 32767)  value = 32767;
            if (value < -32768) value = -32768;

            pOutHub[(size_t)i * LOOPBACK_HUB_CHANNELS + ch] = (SHORT)value;
        }
    }

    if (floatSaved)
    {
        KeRestoreFloatingPointState(&floatSave);
    }
}

VOID
LoopbackConvertFromHub(
    _In_reads_((size_t)FrameCount * LOOPBACK_HUB_CHANNELS) const SHORT* pInHub,
    _In_ ULONG FrameCount,
    _In_ const WAVEFORMATEXTENSIBLE* pDstFormat,
    _Out_writes_bytes_((size_t)FrameCount * pDstFormat->Format.nBlockAlign) BYTE* pOut
    )
{
    WORD  bits           = pDstFormat->Format.wBitsPerSample;
    ULONG bytesPerFrame   = pDstFormat->Format.nBlockAlign;
    ULONG bytesPerSample  = bytesPerFrame / LOOPBACK_HUB_CHANNELS;
    BOOLEAN isFloat       = LoopbackIsFloatFormat(pDstFormat);

    KFLOATING_SAVE floatSave;
    BOOLEAN floatSaved = FALSE;
    if (isFloat)
    {
        floatSaved = NT_SUCCESS(KeSaveFloatingPointState(&floatSave));
    }

    for (ULONG i = 0; i < FrameCount; ++i)
    {
        BYTE* pFrame = pOut + (size_t)i * bytesPerFrame;

        for (ULONG ch = 0; ch < LOOPBACK_HUB_CHANNELS; ++ch)
        {
            SHORT sample  = pInHub[(size_t)i * LOOPBACK_HUB_CHANNELS + ch];
            BYTE* pSample = pFrame + (size_t)ch * bytesPerSample;

            if (isFloat)
            {
                if (floatSaved)
                {
                    *(float UNALIGNED*)pSample = (float)sample / 32768.0f;
                }
                else
                {
                    RtlZeroMemory(pSample, bytesPerSample);
                }
            }
            else
            {
                switch (bits)
                {
                case 8:
                    pSample[0] = (BYTE)((sample / 256) + 128);
                    break;

                case 16:
                    *(SHORT UNALIGNED*)pSample = sample;
                    break;

                case 24:
                    {
                        LONG v = ((LONG)sample) << 8;
                        pSample[0] = (BYTE)(v & 0xFF);
                        pSample[1] = (BYTE)((v >> 8) & 0xFF);
                        pSample[2] = (BYTE)((v >> 16) & 0xFF);
                    }
                    break;

                case 32:
                default:
                    *(LONG UNALIGNED*)pSample = ((LONG)sample) << 16;
                    break;
                }
            }
        }
    }

    if (floatSaved)
    {
        KeRestoreFloatingPointState(&floatSave);
    }
}

//=============================================================================
// Resampler
//=============================================================================

VOID
LoopbackResampleStereo16(
    _In_reads_((size_t)SrcFrames * LOOPBACK_HUB_CHANNELS) const SHORT* pSrc,
    _In_ ULONG SrcFrames,
    _In_ ULONG SrcRate,
    _Out_writes_((size_t)DstFrames * LOOPBACK_HUB_CHANNELS) SHORT* pDst,
    _In_ ULONG DstFrames,
    _In_ ULONG DstRate
    )
{
    if (SrcFrames == 0 || SrcRate == 0 || DstRate == 0)
    {
        RtlZeroMemory(pDst, (size_t)DstFrames * LOOPBACK_HUB_CHANNELS * sizeof(SHORT));
        return;
    }

    // Q16.16 fixed-point step: how many source frames to advance per output frame.
    ULONGLONG step = ((ULONGLONG)SrcRate << 16) / DstRate;
    ULONGLONG pos  = 0;

    for (ULONG i = 0; i < DstFrames; ++i)
    {
        ULONG srcIndex = (ULONG)(pos >> 16);
        ULONG frac     = (ULONG)(pos & 0xFFFF);

        if (srcIndex >= SrcFrames - 1)
        {
            // Not enough source samples left to interpolate; hold the last one.
            srcIndex = SrcFrames - 1;
            frac     = 0;
        }

        for (ULONG ch = 0; ch < LOOPBACK_HUB_CHANNELS; ++ch)
        {
            LONG s0 = pSrc[(size_t)srcIndex * LOOPBACK_HUB_CHANNELS + ch];
            LONG s1 = ((srcIndex + 1) < SrcFrames) ? pSrc[(size_t)(srcIndex + 1) * LOOPBACK_HUB_CHANNELS + ch] : s0;

            LONG interpolated = s0 + (((s1 - s0) * (LONG)frac) >> 16);

            pDst[(size_t)i * LOOPBACK_HUB_CHANNELS + ch] = (SHORT)interpolated;
        }

        pos += step;
    }
}
