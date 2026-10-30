# DX Cluster Plugin — How It Works (+ Bug-Hunt Backlog)

> Sources: `SDRunoPlugin_DXCluster/` — `SDRunoPlugin_DXCluster.h/.cpp`, `SDRunoPlugin_DXClusterUi.h/.cpp`, `SDRunoPlugin_DXClusterForm.h/.cpp`, `SDRunoPlugin_DXClusterProject.cpp`, `dllmain.cpp`, `.vcxproj`.
> Purpose: telnet DX-cluster client that draws spots as spectrum (SP1) annotations.
> Plan context: read this before searching for bugs / patching. §7 lists concrete suspects with file:line.

## 1. What it does (30s version)

1. User enters `Cluster host:port`, `Callsign`, `Timer (min)`, `Response`, `Baseline`, `Colour` in a nana form.
2. On `Start`, plugin opens a blocking TCP socket to the cluster, does a minimal telnet login (`wait for "ogin: " → send "CALLSIGN\r\n" → wait for Response substring`), then loops on `recv()`.
3. Every buffer containing `"DX de"` is parsed as a fixed-column spot line → `(callsign, freqHz, timeUTC HHMM)` → appended to an intrusive singly-linked list (`DXEntry {callsign,freq,timeUTC,style,next}` with `head/tail`).
4. Plugin is also an `IUnoAnnotator`. SDRuno periodically calls `AnnotatorProcess(items)`; plugin walks the list and pushes one `IUnoAnnotatorItem {frequency,text=callsign,power,rgb,style}` per entry. Overlapping nearby freqs are stacked vertically (`baseline + count*5`), first-in-window gets `MarkerAndLine`, rest get `Marker`.
5. A 1 s nana timer updates `Status: Running - DX count: N`. `Stop` / form close / `ClosingDown` closes the socket, joins the worker, unregisters the annotator.

It does **not** use stream/audio observers — only `RegisterAnnotator`, `GetSampleRate`, `Get/SetConfigurationKey`, `RequestUnload`. See `INCLUDE_GUIDE.md §4` for those APIs.

## 2. Architecture — 3 layers

```
SDRuno core
  │ HandleEvent(ev) ──────────────┐
  │ AnnotatorProcess(items) ───┐  │
  │ GetSampleRate(0)           │  │
  ▼                            ▼  ▼
SDRunoPlugin_DXCluster (IUnoPlugin + IUnoAnnotator)  [SDRunoPlugin_DXCluster.h:16-18]
  │ m_controller, m_worker:thread*, m_lock, m_started
  │ head/tail/current/prev/annocurr/freqcurr: DXEntry* (raw, public!)
  │ WorkerFunction() = socket+parse loop
  │ processDX(), add_DXEntry(), olderThanTimer(), checkLocalFreq()
  │ Start/StopDXCluster(), Start/StopAnnotator(), UpdateSampleRate()
  ▼
SDRunoPlugin_DXClusterUi (own std::thread + nana form host)  [Ui.h:18-63]
  │ m_parent ref, m_form:shared_ptr<Form>, m_started (SEPARATE bool!), m_lock
  │ ShowUi() → new Form → Run() (= nana::exec)
  │ Start/StopDXCluster() wrappers, SaveSettings/Location, HandleEvent, FormClosed
  ▼
SDRunoPlugin_DXClusterForm : nana::form  [Form.h:31]
    textboxes (callsign/cluster/timer/baseline/response), colour combox,
    Start/Save buttons, status label, 1 s m_timer
```

DLL entry: `dllmain.cpp` (saves `HMODULE`), `SDRunoPlugin_DXClusterProject.cpp` exports `CreatePlugin / DestroyPlugin / GetPluginApiLevel (=2)`.

Threading (3 threads minimum):

| Thread | Runs | Touches |
|--------|------|---------|
| SDRuno UI/DSP | `HandleEvent`, `AnnotatorProcess`, `UpdateSampleRate` trigger | linked list (read), `m_form->GetBaseline/GetColourIndex` |
| Ui thread (`Ui::m_thread`) | `ShowUi → Form::Run → nana::exec` | form widgets |
| Worker (`Plugin::m_worker`) | `WorkerFunction` socket loop + `processDX/add` | linked list (write), `sock`, `m_started`, `responseMatched` |
| + nana timer callback | 1 s status update, auto-stop repair | `m_parent.DXCount()`, `StartButtonClicked(dummy)` |

There is **no locking** between SDRuno-thread reads and worker-thread writes (see §7).

## 3. Lifecycle / sequence

### Start (happy path)

```
Form.Start click → checkCallsign (stub true) → checkResponse (stub true)
 → checkTimer (clamp 1..20) → checkCluster (split "host:port" on ":")
 → Ui::StartButtonClicked(addr,port,call,mins,resp)
 → Ui::StartDXCluster (guard Ui::m_started, lock) → Plugin::StartDXCluster
   → Plugin::StartDXCluster (guard Plugin::m_started, lock)
     → m_started=true; RegisterAnnotator(this)
     → new thread(WorkerFunction)
 → Form: button→"Stop", disable inputs, StartTimer()
```

Note `Ui::m_started` and `Plugin::m_started` are **two independent bools** that must stay in sync (`Ui.cpp:154-174`).

### Worker (socket) — `SDRunoPlugin_DXCluster.cpp:223-340`

```
WSAStartup → socket() → gethostbyname(cAddr) → connect()
→ if callsign.len>1: waitforstring(sock,"ogin: "); send("CALLSIGN\r\n");
     if response.len>1: waitforstring(sock, response) else responseMatched=true
→ while(m_started && responseMatched):
    inner while(1):
      recv() → if SOCKET_ERROR: m_started=false
      if "DX de" in buffer: processDX(buffer); clear buffer
→ closesocket + WSACleanup
```

`waitforstring` / `sendstring` are tiny blocking helpers (`cpp:172-221`).

### Stop

```
Form.Start click while running → Ui::StartButtonClicked(dummy "a","1","b",1,"dxspider")
 → Ui::StopDXCluster → Plugin::StopDXCluster:
     StopAnnotator() (= UnregisterAnnotator)
     m_started=false; closesocket(sock); m_worker->join(); delete
 → Form: button→"Start", enable inputs, StopTimer()
```

Also: `Ui::HandleEvent(ClosingDown) → FormClosed → StopDXCluster + RequestUnload`; `Form destroy event → FormClosed`; `~Ui → StopDXCluster + exit_all + join`.

### Annotate — `cpp:48-89`

Called by SDRuno at redraw rate (not by plugin):

```
annocurr=head; while(annocurr):
  im.frequency=entry.freq; im.text=entry.callsign
  count=checkLocalFreq(freq,callsign)   // O(n) scan for same-window dupes
  im.power = form.Baseline + count*5
  im.rgb = colors[form.ColourIndex]
  im.style = (power==baseline) ? MarkerAndLine : Marker
  items.push; annocurr=next
dxCount = items.size()
```

`checkLocalFreq` (`cpp:91-120`): window = `sampleRate/100` if ≥2 MHz else `sampleRate/20`; counts entries with `|freq-other|<window` before the first same-callsign match. `sampleRate` comes from `UpdateSampleRate() = GetSampleRate(0)` (`cpp:486-489`), only refreshed on `SampleRateChanged` event (`Ui.cpp:217-219`).

### Persistence — keys `DXCluster.*`

`Ui.cpp:64-152,197-203,256-279`: `X,Y` (window pos, saved on `SavingWorkspace`), `Callsign,Timer,Cluster,Colour,Baseline,Response` (saved on Save button). `Form::Setup` loads all at construct (`Form.cpp:218-227`). Defaults: Timer 5, Colour 1 (yellow), Baseline -70, Cluster `hrd.wa9pie.net:8000`, Response `dxspider`.

## 4. Network protocol + parsing

Telnet, plain TCP, no TLS. Login dance is `ogin: ` prompt–based (works for `login:`/`Login:`). Response match is substring (`strstr`).

Spot line assumed fixed-column, 80-col, e.g.:

```
DX de RW7F:		21190.0  R3UG         TNX                            0849Z
```

`processDX` (`cpp:342-441`):

```
find "DX de" → sfreq = substr(dxdepos+14,10) → strip spaces → stod kHz → *1000 → Hz
               scsign = substr(dxdepos+26,10) → strip spaces
               stimeZ = substr(dxdepos+70,4)  // HHMM, the "0849" in "0849Z"
→ if list empty: add_DXEntry
→ else: walk list; unlink any node with same callsign OR olderThanTimer(node.time)
        then add_DXEntry(new)
```

`olderThanTimer` (`cpp:463-484`): `HHMM` → mins-since-midnight vs current UTC (`gmtime`), expired if `now - entry > timeMinutes`. Has explicit `TODO: cope for 24 -> 0 hour case` — midnight wrap is unhandled.

## 5. UI details worth knowing for debugging

* Form size 297×240, custom SDRuno skin (header/min/close bitmaps), version string `V1.6` (`Form.cpp:12`).
* Start-button validation: callsign/response checks are commented out (always `true`); only timer range + `host:port` colon-split are enforced (`Form.cpp:389-462`). Invalid cluster turns box red and aborts (but does **not** show a message; user sees nothing happen).
* Stop is invoked with dummy args (`"a","1","b",1,"dxspider"`) both from the running-state Start handler (`Form.cpp:351`) and the timer watchdog (`Form.cpp:377`). It works only because `Ui::StartButtonClicked` branches on `Ui::m_started` and ignores args when stopping — fragile if flags desync.
* 1 s timer only updates the status label + repairs a stuck `"Stop"` caption; it does **not** expire old entries (expiry is lazy, on next `processDX`).

## 6. Build / files

* `vcxproj`: Release Win32 = DLL (`v141_xp`), Release x64 = DLL (`v141`), Debug = EXE (local test harness). Includes `../nana/include;../include`, links `nana_v141_Release_x86.lib`. Note x64 config still references `nana...x86.lib` and `../../plugins/...` paths — likely stale (see backlog).
* `resource.h/.rc`: skin bitmaps. `DXClusterPluginHelp.docx`: user help (not reviewed here).

## 7. Bug-hunt backlog (for the patch phase — not fixed yet)

> STABILITY FINDING (2026-10-04, verified over 3 hang incidents + dump
> analysis): **never call GetSP1Min/MaxFrequency/Power from plugin code.**
> Every build that queried SP1 state hung SDRuno within ~1–2 min — once
> from the AnnotatorProcess redraw path, twice from a 1/s timer thread —
> while builds with zero SP1 queries ran for hours (2000+ annotator calls).
> Dump showed no plugin code on any thread; all threads parked in routine
> waits while annotator dispatch went silent with a healthy network thread.
> Prime suspect is re-entrancy/deadlock inside SDRuno's SP1 getters.
> Antenna placement, clamping and in-span filtering that need the SP1 span
> are therefore DISABLED; UpdateSpanCache() is a documented no-op and the
> annotator uses baseline-anchored bottom-up/top-down placement only.
> Update (2026-10-06/07): after capping annotator output at the newest 40
> items, making all anchors degenerate-free, and moving all widget reads
> off the redraw path into a timer-thread cache, the plugin ran 2+ days
> with full spot lists and no hang. Which of the three did it is unknown;
> all three stay.

Ordered by crash/lockup likelihood. Verify each with a repro before fixing.

### P0 — likely crashes / corruption

1. **`waitforstring` precedence bug** — `cpp:183`: `while (res = recv(sockfd, buffer+pos, sizeof(buffer)-pos, 0) > 0)`. `>` binds first, so `res` is 0/1, `pos += res` adds 1 byte per recv, buffer fills with gaps. Should be `while ((res = recv(...)) > 0)`. Consequence: login handshake almost never matches correctly / garbage buffer. Fix + unit-test with fake socket data.
2. **List-removal corruption + leak** — `processDX:412-437`: on match it relinks but never `delete`s, then `prev = current` points `prev` at the *detached* node, so the next unlink writes to a dead chain; `tail` update breaks on mid-list delete. Long run = leak + corrupted overlay / crash in `AnnotatorProcess`. Needs rewrite (or `std::list` + lock) with ASan/Helgrind test.
3. **No socket-API null/error checks** — `WorkerFunction:228,247-251`: `stoi(cPort)` unguarded (throws on `""`/bad port → `terminate`); `gethostbyname` return never null-checked (bad hostname → AV on `server->h_addr`); `inet_addr(server->h_addr)` misuses binary as string (then overwritten by `memcpy` — dead/wrong code); `gethostbyname` is deprecated/non-thread-safe. Replace with `getaddrinfo` + validation.
4. **Stack overflow via `sprintf`** — `cpp:271-275`: `char csTmp[16]; sprintf(csTmp,"%s\r\n",callsign)` overflows on long callsign; `char wsTmp[255]; sprintf(wsTmp,"%s",response)` unbounded. Use `snprintf` + length caps.
5. **Data race on linked list** — `AnnotatorProcess` (SDRuno thread) walks `head` with no lock while worker mutates it; `annocurr/freqcurr/head/tail` are shared mutable + public (`h:50`). Also `GetBaseline/GetColourIndex` poke nana widgets off the UI thread. Add `m_lock` (or snapshot vector under lock) + confirm nana thread-affinity.
6. **`StopDXCluster` shutdown race** — `cpp:151-164` + `Ui.cpp:176-195`: `closesocket(sock)` from UI thread while worker blocks in `recv`; `m_worker->join()` with no null check (double-stop → AV); global `sock` (`cpp:20`) shared across instances; `m_started=false` ordering vs `recv` error path. On Windows `closesocket` does not reliably unblock `recv` — prefer `shutdown(sock,SD_BOTH)` + `join` + null-guard + per-instance socket.

### P1 — wrong behavior / hangs

7. **`responseMatched` not reset on restart** — `StartDXCluster:132-149` never clears it; second Start after a prior success skips login wait even for a new server. Reset to `false` at start.
8. **Multi-line coalescing loss** — `WorkerFunction:297-333`: one `recv` may hold 2+ `DX de` lines; code calls `processDX(buffer)` once then clears, dropping the rest. Also `strstr(buffer,"DX de")` on a partial line at buffer end parses garbage. Needs line-buffering on `\n`.
9. **`res==0` (orderly close) spins forever** — inner `while(1)` only breaks on `!m_started`; graceful close with `res==0` loops calling `recv` busy. Break + set `m_started=false` + update status.
10. **Midnight expiry bug (known TODO)** — `olderThanTimer:463-484`: `totalMinsUTC - totalMins > timeMinutes` goes hugely negative across midnight → entries never expire (or expire instantly if `timeMinutes` large). Also `gmtime` (static buffer, non-thread-safe) → `gmtime_s`; `stoi` throws on short/malformed `timeUTC` → uncaught in worker.
11. **`abs` on `long long`** — `checkLocalFreq:112`: `abs(freq-other)` truncates to `int`; use `llabs`. Plus O(n²) redraw cost (annotator × overlap scan) — cache or index if list grows.
12. **Duplicated `m_started`** — `Plugin::m_started` vs `Ui::m_started` can desync (e.g. worker sets plugin false on socket error, Ui still true → UI says Running forever; timer watchdog then force-stops with dummy args). Single source of truth + error callback to UI.

### P2 — polish / robustness / build

13. **Unvalidated numeric textboxes** — `Form::GetBaseline:99-103` (`stoi` unguarded), `GetTimerText→stoi` (`Form.cpp:452`), `LoadX/Y/Timer/Colour/Baseline` (`Ui.cpp:64-130`, `stoi` unguarded) — non-numeric ini or typing crashes. Wrap in `try`/default + validate range.
14. **List never freed** — no traversal `delete` in destructor/`StopDXCluster`; repeated start/stop + unload leaks all `DXEntry`. Add `clearList()` + call on stop/destroy.
15. **`sendstring` ignores partial send** — `cpp:217-221` single `send()` may send short; loop until `len` sent or error.
16. **Fixed-column parse is brittle** — offsets `+14/10, +26/10, +70/4` assume one cluster format; other servers (CC Cluster, AR-Cluster) differ → silent misparse. Log rejected lines + consider regex/token parse with validation.
17. **Build file drift** — x64 config links `nana_v141_Release_x86.lib` and points at `../../plugins/...` (`vcxproj:114-120`) while Win32 uses `../...`; Debug builds EXE not DLL. Verify both platforms build from clean checkout.
18. **Dead validation stubs** — `checkCallsign/checkResponse` always true with checks commented out (`Form.cpp:389-396,455-462`). Decide: enforce or remove + update help doc.
19. **Status UX on failure** — connect/login failures just set `m_started=false` with no message; form still shows `Stop` until timer flips it. Surface error string to `statusLbl`.
20. **`sampleRate` staleness** — init 2 MHz, only updated on `SampleRateChanged`; if plugin starts after SDRuno already streaming at other BW, window calc is wrong until next change. Call `UpdateSampleRate()` in `StartDXCluster`.

### Suggested patch order

1. Repro harness: fake telnet server feeding the 3 sample lines from `processDX` comments + multi-line + partial-line + bad-host cases.
2. Fix P0 #1–#6 (socket + list + locking) — these are crashers.
3. Fix P1 #7–#12 (restart, coalescing, midnight, `llabs`, single `m_started`).
4. Then P2 hardening + build verification (Win32+x64 Release).

## 8. Quick reference for patching

| Ask | Where |
|-----|-------|
| Socket connect/login | `SDRunoPlugin_DXCluster.cpp:223-291` |
| Recv/dispatch loop | `cpp:293-340` |
| Login-wait helper (precedence bug) | `cpp:172-215` |
| Spot parse + list prune | `cpp:342-441` |
| List append | `cpp:443-461` |
| Expiry | `cpp:463-484` |
| Annotator overlay | `cpp:48-89`, overlap `cpp:91-120` |
| Start/stop + annotator reg | `cpp:122-164`, `Ui.cpp:154-195` |
| Events (sample-rate, save, close) | `Ui.cpp:205-239` |
| Config keys | `Ui.cpp:64-152,197-203,256-279` |
| Form validation + timer | `Form.cpp:323-462` |
| Plugin factory | `SDRunoPlugin_DXClusterProject.cpp` |
| Globals to eliminate | `cpp:18-25 (wsa/sock)`; `h:50-51 (raw list ptrs)` |
