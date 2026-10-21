# F5Low debug / FG correction — installed build

Installed in PCSX2 and MGR Host64 on 2026-10-20. Open the OptiScaler menu using your configured shortcut, then **Neural Rendering → Optical F5Low → Advanced → Show the motion and trust pictures**. Select the desired diagnostic; Enlarge preview and Fullscreen preview use the same corrected transport. Escape exits the enlarged/fullscreen diagnostic. Normal/depth uses the original selected depth source, with normals on the left and depth on the right.

For the tested Silent Hill 3 / PCSX2 Vulkan SDR output, use **Colour Encoding: Auto** (resolves to tone-mapped sRGB). Choosing a different encoding changes NR's input interpretation; its temporal history now resets automatically when the effective domain changes.

Your presets, depth calibration and prior FG setting were preserved. Enable the FG backend you want to compare. The user confirmed the preview and fullscreen with XeFG. Native DX11 and Vulkan runtimes also exercised XeFG/FSR FG; DLSSG was unavailable in this RTX 3080 test. See AUDIT-EN.md for exact scope, remaining validation limits and complete synthetic costs.
