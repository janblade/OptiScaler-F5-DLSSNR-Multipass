# Plugins page

The **Plugins** page in the OptiScaler menu (after Misc) shows what the plugin loader did this session and lets you
switch plugins on or off. Every change applies at the **next start**: tick or untick, press Save Settings, restart the
game. A changed row says "restart to apply" until then.

## What is listed

- **Built-in**: the MFG unlock for XeFG, RTX 40 and RTX 20/30. It is part of OptiScaler, so it is listed even when the
  plugins folder is empty. It is **off by default**. Each family has a mode: Off, Detect (only where this GPU needs the
  unlock) or On. The row shows what the family actually did (active, stood aside and why, waiting for a provider).
- **Loaded from plugins folder**: every `*.asi` file in the plugins folder. Unticking a file lists its name in
  `[Plugins] DisabledAsi`; the file itself is never renamed or changed. Files with `-loadlate` in the name load after
  `[Plugins] LateAsiPluginsDelay` seconds and show "waiting" until then.
- **Loaders**: Special K, ReShade and the custom amdxc64 for RDNA 2, with whether they really loaded.

`Load ASI plugins` (`[Plugins] LoadAsiPlugins`) is the master switch at the top. With it off the folder is still
listed, so you can see what would load.

## What the states mean

| State | Meaning |
|---|---|
| loaded / loaded late | the file was opened and started |
| waiting | a `-loadlate` file, delay not over yet |
| disabled | named in `DisabledAsi`; the file was not opened |
| not loaded: ASI plugins are off | the master switch is off |
| failed to load (code) | Windows refused the file; `0x7E` module not found, `0xC1` not a valid 64-bit DLL |
| on, not loaded: a frame generation input is on | Special K is on in the ini but not loaded while an OptiFG, DLSSG via Streamline or FSR-FG input is active |

The log has one line per start, `[Plugins] N asi: name state; ...`, with the same states.

## Built-in MFG unlock and an external unlocker

Use one of them. If the built-in unlock is on and the "External frame generation / MFG unlocker" switch is on, or an
`.asi` with "unlock" in its name is loading, the page shows a warning. It does not pick a winner.

## ini keys

```
[Plugins]
LoadAsiPlugins=auto      ; master switch, default false
DisabledAsi=auto         ; comma separated file names, default none
[MFGUnlock]
XeFG=auto                ; auto/false = off, detect, true = on
RTX40=auto
RTX2030=auto
```
