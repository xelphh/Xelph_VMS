# Xelph Virtual Audio Driver

A Windows Driver Kit (WDK) kernel-mode audio driver that installs a virtual speaker and a virtual microphone, and routes audio between them like a hardware-free [VB-Cable](https://vb-audio.com/Cable/) — whatever plays on the virtual speaker is automatically heard back on the virtual microphone.

Useful for headless machines, remote desktop / streaming setups, voice-chat testing, and any workflow that needs a "fake" audio device without real hardware.

---

## What's in the box

| Device | Shows up as | Role |
|---|---|---|
| Virtual Speaker | **Speakers (Xelph)** | Playback endpoint — anything played to it is captured internally |
| Virtual Microphone | **Microphone (Xelph)** | Recording endpoint — plays back whatever the virtual speaker rendered |

### Speaker → Microphone loopback

This is the headline feature: audio rendered to the virtual speaker is converted to a fixed internal format (16-bit stereo, 48 kHz), pushed through a ring buffer, and pulled back out on the microphone side — resampled and reformatted to whatever the microphone stream actually negotiated. No cable, no third-party mixer, no extra software.

- Enabled by default.
- If nothing is currently playing, the microphone reads back silence rather than random Statics.
- For best fidelity, set both devices to the same sample rate/bit depth in Windows' Sound Control Panel "Advanced" tab — the driver resamples automatically otherwise, but a matched rate avoids the extra conversion step.
- To check it's working: set your default speaker to **"Speakers (Xelph)"**, then go to the sound control panel → Recording tab, and you'll see **"Microphone (Xelph)"** VU level moving. You can also listen to it live from the Listen tab (make sure to pick your real headphones/speakers in the playback device dropdown there, not the virtual device).

---

## Compatibility

- **OS**: Windows 10 and Windows 11
- **Architecture**: x64 (tested) any other architecture is not tested if it crashes the pc its not my fault

---

## Installation

Test-signed builds require test signing to be enabled first:

```powershell
bcdedit /set testsigning on
```

*(A production/EV-signed build wouldn't need this step — this project currently ships test-signed only.)*

1. Open **Device Manager** → **Action** → **Add Legacy Hardware**.
2. Choose **Install the hardware that I manually select from a list (Advanced)**.
3. Choose **Sound, video and game controllers**.
4. Choose **Have Disk...** and point it at `Xelph_VMS.inf` from the build output.
5. Complete the wizard, then trust the driver's test certificate if Windows asks (see below).

If the driver's certificate isn't trusted yet, install it into both the **Trusted Root Certification Authorities** and **Trusted Publishers** stores:

```powershell
certutil -addstore "Root" Xelph_VMS.cer
certutil -addstore -f "TrustedPublisher" Xelph_VMS.cer
```

### Verifying installation

- **Sound, video and game controllers** → **"Xelph"**
- **Audio inputs and outputs** → **"Speaker/Microphone (Xelph)"**

---

## Usage

1. Set **VS (speaker) by Xelph** as your default playback device (Sound Settings → Output).
2. Set **VM (microphone) by Xelph** as your default recording device (Sound Settings → Input) in whatever app needs the audio — Discord, OBS, a conferencing app, a test harness, etc.
3. Play audio normally. It comes back out the virtual microphone automatically.

### Supported formats

| | Virtual Speaker | Virtual Microphone |
|---|---|---|
| Channels | Stereo | Stereo |
| Bit depth | 16-bit or 24-bit | 32-bit |
| Sample rate | 48 kHz – 192 kHz (negotiated) | 48 kHz |

---

## Configuration

The driver reads a few `REG_DWORD` values from its own `Parameters` registry key at load time:

| Value | Default | Effect |
|---|---|---|
| `DisableLoopback` | `0` (loopback **on**) | Set to `1` to stop routing speaker audio to the microphone — the mic will just output silence. |
| `DisableToneGenerator` | `1` (tone **off**) | Set to `0` to make the microphone emit a test sine tone instead of loopback audio, useful for isolating driver issues from application issues. |
| `DoNotCreateDataFiles` | `1` (off) | Set to `0` to have the driver dump rendered speaker audio to a debug `.wav` file on disk — diagnostic use only. |

---

## Building from source

1. Requires Visual Studio with the **WDK (Windows Driver Kit)** build tools installed and matched to your Windows SDK version.
2. Run `build.bat` from the repository root:

   ```
   build.bat                # Release, x64 (default)
   build.bat debug x64      # Debug, x64
   build.bat release arm64  # Release, ARM64
   build.bat all            # every configuration/platform combination
   ```

3. Output lands in `<Platform>\<Configuration>\Setup\`, containing `Xelph_VMS.sys`, `Xelph_VMS.inf`, and `xelph_vms.cat`.

The solution is `Xelph_VMS.sln`, split into:

- **Core** — driver entry point, adapter/device context, topology + wave miniport engines, per-stream worker
- **Endpoints** — the speaker and microphone's endpoint-specific topology/property handlers and format tables
- **Support** — mixer state, tone generator, the speaker→mic loopback ring buffer, debug `.wav` writer
- **Headers** — shared declarations used across the above
- **Setup** — packages the built driver into a signed, installable driver package

---

## Contact

- **Discord**: xelphh
- **Website**: [Xelph.lol](https://Xelph.lol)

Issues and pull requests are welcome.

---

## Attribution

Used Windows Driver Kit (WDK) and derived from Microsoft's Windows Driver Samples.

- Windows Driver Kit: https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk
