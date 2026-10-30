# SDRuno Plugin API — `include/` Guide

> Source of truth: `include/` (Plugin API v2, requires SDRuno ≥ 1.40.2 per `README.md`).
> 10 headers. This doc explains what each header does and what you can **build** with it.

## 1. Big picture

```
+-----------+   IUnoPluginController   +-----------+
|  SDRuno   | <----------------------> |  Plugin   |
|  core/DSP |   control + data taps    | (your DLL)|
+-----------+                          +-----------+
      | HandleEvent(UnoEvent)  →  notifies plugin of state changes
      | Register*Observer/Processor → plugin taps/modifies signal path
      | Get/Set*               → plugin reads/controls radio
```

Core concepts:

* `channel_t` (`unsigned short`, see `complex.h:9`, `iunoplugin.h:22`) = VRX channel index. Almost every controller call takes a `channel`. Multi-VRX plugins iterate `0 .. GetVRXCount()-1`.
* `Complex { float real, imag; }` (`complex.h:3-7`) = one IQ sample. All IF/IQ paths use this.
* Plugin entry point is a class deriving from `IUnoPlugin`. SDRuno owns an `IUnoPluginController` and passes it to your constructor. You call back into it to observe, process, control, and persist.
* `UnoEvent` is the async notification channel. `HandleEvent()` is where you react.

## 2. File map

| File | Role | What it unlocks |
|------|------|-----------------|
| `iunoplugin.h` | Base class `IUnoPlugin`, `UNOPLUGINAPIVERSION=0x2`, export macros | Plugin identity, lifecycle, event entry |
| `iunoplugincontroller.h` | Central `IUnoPluginController` interface (~50 virtuals) | **Everything**: tap DSP/audio, control radio, VRX, persistence |
| `unoevent.h` | `UnoEvent` + 35 `UnoEventType`s | Reactive UI, auto-retune, state sync, cleanup |
| `complex.h` | `Complex`, `channel_t` | IQ DSP |
| `iunostreamobserver.h` | `StreamObserverProcess(ch, const Complex*, len)` | Passive IQ snooping: scopes, decoders, loggers, detectors |
| `iunostreamprocessor.h` | `StreamProcessorProcess(ch, Complex*, len, bool &modified)` | Active IQ DSP: filters, notch, gain, scrambler, custom demod |
| `iunoaudioobserver.h` | `AudioObserverProcess(ch, const float*, len)` | Audio meters, recorders, decoders (DTMF, FT8, Morse via audio) |
| `iunoaudioprocessor.h` | `AudioProcessorProcess(ch, float*, len, bool &modified)` | Audio FX: EQ, mute, beep injection, squelch gating, vocoder |
| `iunompxobserver.h` | `MpxObserverProcess(ch, const float*, len)` | WFM MPX taps: RDS/RBDS, stereo pilot analysis |
| `iunoannotator.h` | `AnnotatorProcess(vector<IUnoAnnotatorItem>&)` + styles | Spectrum overlay on SP1: flags, boxes, markers, lines |

See `SDRunoPlugin_Template/SDRunoPlugin_Template.h:16-36` for the canonical wiring: `class MyPlugin : public IUnoPlugin` holding a UI form, forwarding `HandleEvent()` to the form.

## 3. `iunoplugin.h` — plugin shell

```cpp
class IUnoPlugin {
  IUnoPlugin(IUnoPluginController& controller);
  virtual const char* GetPluginName() const;
  virtual void HandleEvent(const UnoEvent& ev);
  virtual bool IsEnabled();
protected:
  IUnoPluginController& m_controller;
};
```

What you can do:

* Name the plugin shown in SDRuno (`GetPluginName()`).
* Receive every radio/UI change in `HandleEvent()`.
* Gate processing with `IsEnabled()`.
* Keep `m_controller` for all later calls — observe, control, persist.

This is deliberately thin; all power lives in the controller.

## 4. `iunoplugincontroller.h` — the control surface

This is ~90% of what `include/` offers. Grouped by capability:

### 4.1 Tap / modify signal paths (observer vs processor)

| Register pair | Callback | Data | Use for |
|---------------|----------|------|---------|
| `RegisterStreamObserver / UnregisterStreamObserver(ch, IUnoStreamObserver*)` | `StreamObserverProcess(ch, const Complex*, len)` — read-only | IF IQ, `Complex` array | Spectrum scope, power meter, custom decoder (ADSB/DAB/IQ), signal detector, IQ recorder. Zero risk to audio chain. |
| `RegisterStreamProcessor / UnregisterStreamProcessor(ch, IUnoStreamProcessor*)` | `StreamProcessorProcess(ch, Complex*, len, bool &modified)` — in-place | IF IQ, mutable | Denoiser, notch, AGC assist, frequency shifter, test-tone injector. Set `modified=true` only if you touched the buffer. |
| `RegisterAudioObserver / UnregisterAudioObserver(ch, IUnoAudioObserver*)` | `AudioObserverProcess(ch, const float*, len)` | Demodulated mono float audio | VU meter, WAV recorder, CTCSS/DTMF decoder, SNR logger. See `SDRunoPlugin_DXCluster` pattern for logger-style use. |
| `RegisterAudioProcessor / UnregisterAudioProcessor(ch, IUnoAudioProcessor*)` | `AudioProcessorProcess(ch, float*, len, bool &modified)` | Mutable audio | Audio filter, mute/beep, replay injection, privacy mute. |
| `RegisterMpxObserver / UnregisterMpxObserver(ch, IUnoMpxObserver*)` | `MpxObserverProcess(ch, const float*, len)` | WFM MPX baseband | RDS decoder, pilot-tone (19 kHz) detector, MPX level scope. |
| `RegisterAnnotator / UnregisterAnnotator(IUnoAnnotator*)` | `AnnotatorProcess(vector<IUnoAnnotatorItem>&)` | Spectrum overlay items | Mark stations, DX spots, ADSB hits, scanner hits on SP1. No channel arg — global overlay. |

Practical takeaways:

* **Observer = safe sniffer.** Prefer it for meters, loggers, decoders. Never blocks or alters SDRuno DSP.
* **Processor = inline DSP.** You get a mutable buffer + `modified` flag. Great for custom filtering but must be fast and thread-safe — this runs on the DSP thread.
* You can register per-VRX (`channel`) for stream/audio/mpx, so one DLL can monitor VRX0+VRX1 simultaneously.
* Example precedent: `SDRunoPlugin_ADSB/` uses stream taps + custom demod types (`DemodulatorADSB2/ADSB8`, see below) to decode 1090 MHz frames without touching SDRuno's audio path.

### 4.2 Full radio control (get/set pairs)

All take `channel_t channel` unless noted:

* **Demodulation:** `Get/SetDemodulatorType()` with 16 modes: `None, AM, SAM, NFM, MFM, WFM, SWFM, DSB, LSB, USB, CW, Digital, DAB, IQOUT, ADSB2, ADSB8`. → Build mode scanners, auto-mode switchers, digital-mode helpers.
* **Frequency:** `Get/SetVfoFrequency()`, `Get/SetCenterFrequency()`, `Get/SetSampleRate()`, `GetAudioSampleRate()`, `GetVFOSelect()/SetVFOSelect()`, `GetStepSize()`, `GetSP1Min/MaxFrequency()` → Build scanners, band-hoppers, click-to-tune, DX-cluster follow, frequency bookmarks.
* **Filtering/BW:** `Get/SetFilterBandwidth()` → Adaptive BW, presets per mode.
* **Gain/IF:** `SetIFGRRelative(ch, increment)` (relative step), `GetSNR()`, `GetPower()` → AGC helpers, signal-strength loggers, auto-gain.
* **Squelch:** `Set/GetSquelchLevel()`, `Set/GetSquelchEnable()` → Scanner with squelch break detection.
* **AGC:** `Set/GetAgcMode()` (`Off/Slow/Medium/Fast`), `Set/GetAgcThreshold()` → Auto-AGC profiles.
* **Noise tools:** `Set/GetNoiseBlankerLevel()`, `Set/GetNoiseReductionLevel()`, `Set/GetCwPeakFilterThreshold()`, `Set/GetFmNoiseReductionEnable/Threshold()`, `Set/GetWfmDeemphasisMode()` (`None/50us/75us`) → One-click presets, band-condition adaptors.
* **Audio:** `Set/GetAudioVolume()`, `Set/GetAudioMute()` → Mute-on-scan, scheduled recorder, alarm.
* **VRX management:** `GetVRXCount()`, `Get/SetVRXEnable()` → Multi-channel monitor, VRX scanner.
* **Spectrum (SP1):** `GetSP1Min/MaxFrequency()`, `GetSP1Min/MaxPower()` → Sync overlay with visible span; clamp annotator items to view.
* **MPX/WFM:** `GetMPXLevel()/SetMPXLevel()` → RDS level calibration.
* **Hardware:** `Get/SetBiasTEnable()` (global, no channel) → LNA power control from plugin UI.
* **Streaming state:** `IsStreamingEnabled(ch)` → Guard DSP registration; avoid processing when stopped.

### 4.3 Persistence, lifecycle, misc

* `Get/SetConfigurationKey(key, value)` (`std::string`) → Persist settings across restarts / workspace saves. Use on `SavingWorkspace` event (see below).
* `RequestUnload(plugin)` → Self-unload button (e.g. “Close plugin” in your UI).
* Always pair Register/Unregister. Leaked observers after unload = crash. Unregister on `ClosingDown` and destructor.

## 5. `unoevent.h` — reactive programming

`HandleEvent(const UnoEvent& ev)` receives `ev.GetType()` + `ev.GetChannel()`. 35 types:

* **Tuning:** `FrequencyChanged, CenterFrequencyChanged, VFOChanged, StepSizeChanged, SP1MinFreqChanged, SP1MaxFreqChanged, SP1MinPowerChanged, SP1MaxPowerChanged`
* **DSP chain:** `DemodulatorChanged, BandwidthChanged, SampleRateChanged, StreamingStarted/Stopped, StartRequest/StopRequest, IFGainChanged`
* **Squelch/AGC/NR:** `SquelchEnableChanged, SquelchThresholdChanged, AgcModeChanged, AgcThresholdChanged, NoiseBlankerLevelChanged, NoiseReductionLevelChanged, CwPeakFilterThresholdChanged, FmNoiseReductionEnabledChanged/ThresholdChanged, WfmDeemphasisModeChanged, AudioVolumeChanged, AudioMuteChanged`
* **VRX:** `VRXCountChanged, VRXStateChanged`
* **Lifecycle/persistence:** `SavingWorkspace, ClosingDown`
* **Signal detect:** `VFOSignalPresent / VFOSignalNotPresent` → Ideal for scanner stop/resume logic.
* **HW:** `BiasTEnableChanged`

What this enables: live-synced UIs (no polling), auto-save on `SavingWorkspace`, clean shutdown on `ClosingDown`, scanners driven by `VFOSignalPresent`, retune cascades on `FrequencyChanged`.

Pattern from template (`SDRunoPlugin_Template.cpp:22-25`): plugin forwards `HandleEvent` to its form (`m_form.HandleEvent(ev)`), form updates nana UI. Keep handler light; defer heavy work to worker thread.

## 6. Annotator (`iunoannotator.h`) — draw on the spectrum

```cpp
struct IUnoAnnotatorItem {
  long long frequency; int power; std::string text;
  IUnoAnnotatorStyle style; uint32_t rgb;
  long long lineToFreq; int lineToPower;
};
enum { Flag=1, Box=2, Marker=3, MarkerAndLine=4 };
```

* Fill `items` in `AnnotatorProcess()`. SDRuno draws them on SP1.
* Uses: DX-cluster spots, ADSB callsigns, scanner hits, band-plan labels, signal history trails (`lineToFreq/Power` draws leader line).
* Combine with `GetSP1Min/MaxFrequency()` to cull off-screen items for performance.

## 7. What can you actually build? (capability → API mapping)

| Idea | APIs used |
|------|-----------|
| **IQ recorder / player helper** | `StreamObserver + GetSampleRate + IsStreamingEnabled + VFO/Center freq` |
| **Custom decoder (FT8, DMR, ACARS, ADSB)** | `StreamObserver (or AudioObserver for audio-based modes) + DemodulatorType + FilterBandwidth + SampleRate` — see `SDRunoPlugin_ADSB/` + `dump1090/` as reference |
| **Band scanner with stop on signal** | `SetVfoFrequency + VFOSignalPresent/NotPresent + Squelch + VRXEnable + Annotator for hits` |
| **DX-cluster overlay** | `Annotator + GetVfoFrequency + SetVfoFrequency (click-to-tune) + SP1 span` — see `SDRunoPlugin_DXCluster/` |
| **RDS/RBDS decoder** | `MpxObserver + MPXLevel + WFM mode check` |
| **Audio logger / squelch-gated recorder** | `AudioObserver + SquelchEnable + AudioSampleRate + ConfigurationKey for path` |
| **DSP FX (notch, NR assist, EQ)** | `StreamProcessor or AudioProcessor (set modified=true)` |
| **Auto-tuner / CAT bridge** | `VFO/Center/Demod/BW getters+setters + FrequencyChanged/VFOChanged events` |
| **Signal survey / propagation logger** | `GetPower/GetSNR polling + worker thread + ConfigurationKey` |
| **One-click presets (airband, FM, CW)** | `DemodulatorType + Bandwidth + AGC + NB/NR + Deemphasis + Volume` |
| **Bias-T / hardware toggle panel** | `Get/SetBiasTEnable + BiasTEnableChanged` |
| **Multi-VRX monitor wall** | `GetVRXCount + per-channel observers + VRXStateChanged` |

Anything needing transmit, hardware driver access, or SP2 (not SP1) overlay is **not** exposed here — this API is receive-side control + RX taps + SP1 annotation only.

## 8. Constraints & gotchas

1. **API v2 only.** `UNOPLUGINAPIVERSION 0x2`. Built for SDRuno ≥ 1.40.2. No backward compat shim in `include/`.
2. **Threading:** observer/processor callbacks are real-time. No blocking I/O, no UI calls, no locks held long. Copy buffer and post to worker (template has `WorkerFunction()` + `m_lock` stub for this).
3. **Ownership:** SDRuno owns controller; plugin owns observer objects. Must `Unregister*` before destruction.
4. **No `<string>` include in `iunoplugincontroller.h`** — include `<string>` yourself before it or rely on transitive include; `Get/SetConfigurationKey` needs it.
5. **Windows export macros** (`UNOPLUGINAPI __declspec(dllexport)`, `UNOPLUGINCALL __cdecl` in `iunoplugin.h:3-15`) — build as DLL with `dllmain.cpp` pattern from template.
6. **UI toolkit** in examples is `nana/` (bundled). API itself is UI-agnostic, but template/ADSB/DXCluster all use nana forms.
7. **Details live in** `SDRunoPluginsDeveloperPack.pdf` (repo root) — read it alongside this doc for lifecycle diagrams and install paths.

## 9. Minimal starter sketch

```cpp
#include <iunoplugin.h>
#include <iunoplugincontroller.h>
#include <iunostreamobserver.h>
#include <unoevent.h>

class MyPlugin : public IUnoPlugin, public IUnoStreamObserver {
public:
  MyPlugin(IUnoPluginController& c) : IUnoPlugin(c) {
    c.RegisterStreamObserver(0, this);
  }
  ~MyPlugin() { m_controller.UnregisterStreamObserver(0, this); }
  const char* GetPluginName() const override { return "My Scanner"; }
  void HandleEvent(const UnoEvent& ev) override {
    if (ev.GetType() == UnoEvent::VFOSignalPresent)
      m_controller.GetPower(ev.GetChannel()); // log hit
    if (ev.GetType() == UnoEvent::ClosingDown)
      m_controller.UnregisterStreamObserver(0, this);
  }
  void StreamObserverProcess(channel_t ch, const Complex* buf, int len) override {
    // fast DSP here; copy out, don't block
  }
};
```

Next steps: open `SDRunoPlugin_Template/` for full DLL + nana UI wiring, `SDRunoPlugin_ADSB/ADSBplugin.*` for a real stream-decoder, `SDRunoPlugin_DXCluster/` for annotator + network use.
