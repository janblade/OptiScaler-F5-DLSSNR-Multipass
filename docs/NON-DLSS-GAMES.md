# DLSS-NR in games that have no DLSS

Our OptiScaler DLSS-NR build only works in games that already use an upscaler (DLSS, FSR or
XeSS), because it needs the game's depth and motion information. Many games have none of that.

**[DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder)** fills the gap. It is a free
ReShade add-on that works out the depth and motion itself, then hands a normal DLSS request to our
OptiScaler build, which runs DLSS-NR on it as usual. All our DLSS-NR options (multipass, Reuse,
presets and so on) still work.

> **Experimental.** DLSS5-Feeder officially supports two other DLSS-NR OptiScaler builds. Ours works
> the same way, but it has not been tested widely yet. Please report what you find (see
> [Reporting a problem](#reporting-a-problem)).

---

## What you need

- A **64-bit or 32-bit** game using **DirectX 9, 10, 11 or 12, OpenGL or Vulkan**, with **no DLSS**
  of its own. (If the game already has DLSS, you don't need this guide: use our build on its own.)
- An NVIDIA RTX graphics card and a recent driver.
- Our release zip: **`OptiScaler-DLSSNR-F5-<version>.zip`** from
  [our releases page](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/releases).
  Don't unzip it; the installer does that.
- The **`nvngx_dlssnr.dll`** you already use with our build. If you have none, the installer
  downloads one, but that one only runs on **RTX 50** cards. On an RTX 20, 30 or 40 card, use the
  `nvngx_dlssnr.dll` made for your card.

You do **not** need to install ReShade yourself. The installer does it.

---

## Install (about 5 minutes)

### Step 1: Make a backup

Copy your game folder somewhere safe, or at least note which files are already next to the
game's `.exe`. The installer backs up what it changes, but a copy is the easy way back.

### Step 2: Get the installer

1. Open the [DLSS5-Feeder releases page](https://github.com/jlrouzies-fr/DLSS5-Feeder/releases/latest).
2. Download **`Install-DLSS5Feeder.ps1`**.
3. Put it **in the same folder as the game's `.exe`**.
4. Put our **`OptiScaler-DLSSNR-F5-<version>.zip`** in that folder too.
5. If you have your own `nvngx_dlssnr.dll`, put it there as well.

> ⚠️ Only download DLSS5-Feeder from that GitHub page. Fake copies with malware exist on other
> sites.

### Step 3: Run it

1. In the game folder, **right-click on an empty spot** and choose **Open in Terminal**.
   (On older Windows: hold **Shift**, right-click, **Open PowerShell window here**.)
2. Copy this line, paste it into the window, change the zip name to match yours, and press **Enter**:

   ```
   powershell.exe -ExecutionPolicy Bypass -File .\Install-DLSS5Feeder.ps1 -Consumer OptiScaler -OptiScalerFork Dagherbou -OptiScalerZip .\OptiScaler-DLSSNR-F5-v0.1.21-plain-fp16-reuse-fix.zip
   ```

   If you have your own `nvngx_dlssnr.dll`, add this to the end of the line before pressing Enter:

   ```
    -DlssNrDll .\nvngx_dlssnr.dll
   ```

   > Why "Dagherbou"? That tells the installer to expect a build that ships its own
   > `nvngx.dll_dlssnr.dll`, which ours does. It still installs **our** zip, not Dagherbou's.

3. Confirm the game `.exe` it finds, and answer **yes** to its questions.
4. At the end it checks everything and prints a list. Every line should say **`[ OK ]`**.
   A `[FAIL]` line tells you how to fix it.

**What it did:** installed ReShade, the DLSS5-Feeder add-on, a motion shader (LumeniteFX), and our
OptiScaler renamed to **`winmm.dll`** (sometimes `version.dll`). It also set up `OptiScaler.ini`
for you.

#### Or start from our side: `Install-NonDlssGame.ps1`

`tools/Install-NonDlssGame.ps1` in this repository does Steps 2 and 3 for you, starting from **our**
zip instead of Feeder's installer. It finds our zip and your `nvngx_dlssnr.dll`, downloads Feeder's
installer from its **latest GitHub release** (the newest version each time, and it shows the release tag and
SHA256), checks it still takes the options the script uses, runs it with our zip, then checks our side: the ini
keys and the model. Because the newest Feeder is not read by us before it runs, `-UseReviewedFeeder` switches to
the last version we did read (1.17.0, hash-checked) if a new release breaks something.

```
powershell.exe -ExecutionPolicy Bypass -File .\Install-NonDlssGame.ps1 "C:\path\to\game.exe" -ModelDll .\nvngx_dlssnr.dll -Plan
```

`-Plan` shows what it would do and changes nothing; leave it off to install. After the first launch,
run it again with `-CheckOnly` to read Feeder's and OptiScaler's logs and say whether the neural pass ran.
It has not been run through Feeder's installer end to end yet.

### Step 4: Check two settings

Open **`OptiScaler.ini`** in the game folder with Notepad and make sure these lines say:

```
[DlssNr]
Enabled=true
FinishedPicture=false

[Upscalers]
Dx12Upscaler=dlss

[ProcessFilter]
TargetProcessName=auto
```

**`FinishedPicture` must be `false`.** Otherwise DLSS-NR waits for game data that never comes and
does nothing.

### Step 5: Play

1. Start the game.
2. In the game's own settings, turn **MSAA / SSAA off**.
3. Press **Home** to open ReShade. Make sure these two are ticked, **in this order**:
   1. `LUMENITE: Kernel 2.0`
   2. `DLSS 5 Feed`
4. Press **Insert** to open OptiScaler's menu. The DLSS-NR section there works exactly as in any
   other game.

---

## Did it work?

Two log files are written next to the game `.exe`. `dlss5-feed.log` should contain lines like:

- `NGX calls are routed through OptiScaler DLSS-NR (winmm.dll)`: it found our build. ✅
- `neural model (feature 18) loaded`: DLSS-NR is running. ✅
- It may say **"forwarder build (Dagherbou…)"**. That is normal for our build and not a problem.

In `OptiScaler.log`, DLSS-NR lines should appear once you're in game.

For a quick visual check, toggle **DLSS 5 Feed** off and on in ReShade (Home). The picture should
change.

---

## Common problems

| What you see | What to do |
|---|---|
| The picture never changes | Check `FinishedPicture=false` and `Enabled=true` in `OptiScaler.ini` (Step 4). |
| `dlss5-feed.log` says the **driver** answered, not OptiScaler | OptiScaler did not load. Check that `winmm.dll` (or `version.dll`) is next to the game `.exe`, and that `TargetProcessName=auto`. |
| `neural model … NOT loaded` | `nvngx_dlssnr.dll` is missing, or isn't the right one for your card. `OptiScaler.log` says which. |
| Weird smearing, ghosting or wobbly edges | The motion is *estimated*, not from the game, so some ghosting is expected. Try other DLSS-NR settings in the Insert menu. |
| Flickering when **Model resolution** is below 100% | Set Model resolution back to **100%** and tell us. |
| ReShade's **Motion vectors** section is red | `LUMENITE: Kernel 2.0` must be enabled and placed **above** `DLSS 5 Feed`. |
| Game won't start after installing | Delete `winmm.dll` (or `version.dll`) from the game folder to rule OptiScaler out, then report it. |
| Nothing happens, and you also have `renodx-dlss5*.addon64` or another DLSS 5 neural add-on in the folder | Remove it. **Only one** thing may run the neural pass, and here that's our OptiScaler. |

### Vulkan games only

Turn **NVIDIA Smooth Motion off** for Vulkan (it can't work with this). DirectX games are fine
either way.

---

## Uninstall

Restore your backup from Step 1, or delete these from the game folder:
`winmm.dll` / `version.dll` (our OptiScaler), `OptiScaler.ini`, the `OptiScaler\` folder,
`nvngx.dll_dlssnr.dll`, `dlss5-feed.addon64`, `dlss5-feed.cfg`, ReShade's `dxgi.dll` (or
whichever ReShade DLL was installed), `ReShade.ini`, `ReShadePreset.ini`, and the
`reshade-shaders\` folder.

---

## Reporting a problem

Please include:

- the game name and whether it's DirectX 11, DirectX 12 or Vulkan;
- your graphics card;
- `dlss5-feed.log` and `OptiScaler.log` from the game folder;
- a screenshot of the installer's final check list.

**Report it to us first**, not to the DLSS5-Feeder author. Our build isn't on their supported
list yet.

---

*DLSS5-Feeder is made by Jean-Laurent Rouzies (MIT licence). LumeniteFX is by umar-afzaal. Thanks
to both. This guide only explains how to use their work with our build.*
