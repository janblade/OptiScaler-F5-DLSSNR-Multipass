LUTs for DLSS-NR (Neural Rendering > NR Input > LUT)

Put 3D LUT files (.cube) in this folder, press Rescan in the menu and pick one. Any other path also works
through [DlssNr] LutFile in OptiScaler.ini. 1D LUTs and chained log-to-display LUT packs are not supported.

The LUT is applied to the image NR is given, before the model sees it, so the same cautions apply as for
any grade: a strong look can shift auto-exposure and skin-tone masking. Start with LUT strength around 0.3.

Files here
  identity_33.cube, shift_33.cube   Test LUTs made for this project (no change / an obvious shift).
  AgX-*.cube, JP2499DRT*.cube       Film-style looks baked for this feature, with the credits in each file
                                    and in docs/CREDITS.md. They take a display picture in and give a display
                                    picture out; the AgX and JP2499DRT looks expect a log input, so each file
                                    has the sRGB -> linear -> ARRI LogC4 steps built in.
