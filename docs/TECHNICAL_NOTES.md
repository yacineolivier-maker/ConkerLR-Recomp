# Technical notes

What this port had to solve beyond the SDK, for anyone working on Conker: Live & Reloaded or another D3D8 LTCG
title. Addresses are those of the European `default.xbe` (SHA-1 `68a37f0d...dd551254`).

## The executable

- XDK 1.0.5849 for every library, Direct3D linked with **LTCG**: many D3D setters are inlined in the game's
  code and write the XDK's tables or push NV2A methods directly. HLE functions alone never see that state.
- `config/game.toml`: symbols written by `xbe_symbols`, then `[abi]` and `[data]` entries established by hand
  (LTCG calling conventions, with the local `none` location for an argument LTCG removed) and `[[hook]]`s.

## State written by inlined code

The most frequent family of bugs. Each time, the fix was to re-read the XDK table or handle the NV2A method.

| Symptom | Cause | Fix |
|---|---|---|
| Characters' 32x32 lighting maps black | `SetStreamSource` inlined into `D3D_Streams` (0x547158, 12 bytes per stream), vertex arrays pushed by the game itself (methods 0x1720 address, 0x1760 format) | draws re-read the table; a declaration reaching past the stream stride reads the pushed arrays (`StreamsFromArrays`) |
| Black triangles around projected shadows | texture border colour pushed inline (method 0x1B24) | read from the register |
| Bump matrices at zero (water, glass) | setter 0x537AD0 inlined, pushes 0x1B28-0x1B3C | table re-read, registers used in push buffers |
| Stale combiners / textures in push buffers | XDK `SetPixelShader` / `SetTexture` keep device fields (+0x784..+0x790, +0xC + stage * 4) and dirty flags that `RunPushBuffer`'s lazy state setter reads | HLEs maintain the same fields |
| Rain drops black | the refracted screen copy (0x3EAFB0) pushes immediate-mode vertices as `SET_VERTEX_DATA2S` / `4S` | push buffer walker handles 2 and 4 short vertices |
| Fog colour over whole plants | push buffer draws take fog from methods 0x29C / 0x2A4 / 0x9C0, not the D3D render states | read from the methods |

## Textures

- Swizzled / DXT texture formats pushed without their log2 sizes were treated as linear: the size now comes
  from the image rect.
- Compressed **3D textures**: the NV2A stores DXT blocks interleaved in groups of four slices; they are
  reordered slice by slice.

## Presentation and timing

- `Present(1)` in borderless mode with vsync could stop returning the GPU queue (one launch in two). Frames are
  presented with `Present(0, 0)` after a vertical blank awaited on a thread (`WaitForVBlank`, 40 ms at most).
- The **New Game loading screen** (0x2AD3B0) is drawn into the frame buffer without a `Swap`: the console shows
  its frame buffer continuously. A frame buffer drawn without a present for 100 ms is presented
  (`ShowDrawnDisplayBuffer`).
- The loading state machine (0x2AADC0 / 0x2AB260) waits for the menu music track to end (about 30 s, by
  design), while the game thread polls its files in a tight loop (`NtQueryInformationFile`, 0x627E0). That loop
  starved the XACT thread; a short yield on back-to-back queries fixed it. Caching file information instead
  made the stall permanent.
- Controllers must appear about one second after `XInitDevices`, like the console's USB enumeration: the game
  only enables a player's stick on a plug-in event (0x3D1C20).

## Levels

- Level name table at 0x69CF90 (Frontend 29, ThroneRoom 24, TrainingLevel 41 = Hungover, Windmill 6,
  Canyon 28, BeachRunup 4...). Current level [0x7FA1F8], requested [0x7FA1F4], entry point [0x7FA1E4] /
  [0x7FA1E8]; request function 0x2A94A0, level loop 0x2A93D0, load point 0x2A9442.
- `XBE_NIVEAU=<name or number>` (hook at 0x2A9442, `src/game/diag_hooks.cpp`) loads that level in place of the
  first one after the menu; `XBE_ENTREE` picks its entry point.

## Language

`XC_LANGUAGE` (non-volatile setting 7, EEPROM offset 0xE8) is what the game reads; the European disc carries
`text\French`, `German`, `Italian`, `Spanish` and `Japanese`. Voices are English only.

## Performance

BeachRunup sends 13,000 small push buffer draws per frame. What mattered: no `getenv` per draw, shader
programs and combiners memoised, shader resource views and constants reused when unchanged, and a descriptor
ring large enough for one frame (it used to wrap within a frame and overwrite views in use, which also crashed
the driver at 200 % resolution).

## Rendering additions (`src/common/rendu.cpp`)

A `gpu::Extension` adds soft shadows (the game's shadow textures blurred, made at up to 1024 pixels), GTAO
ambient occlusion (projection read from the c3-c6 constants of Rare's vertex programs, applied at the first
transparent draw so fur layers and lighting maps are complete), SMAA and CAS sharpening at present.

## Diagnostics

| Variable | Use |
|---|---|
| `XBE_CAPTURE_AT=<s>x<n>` + `XBE_CAPTURE_DIR` | full frame captures: every draw's state in `frame.txt`, targets, textures, shaders |
| `XBE_CAPTURE_STEPS=n,m` | the target before those draws |
| `XBE_RECORD_AT=<s>x<n>` | n consecutive final frames |
| `XBE_PROFILE=<start>,<seconds>` | sampling profiler of every thread, symbols from the PDB |
| `XBE_MEMDUMP_AT=<s>`, `XBE_MEMDUMP_RANGE=addr:size` | memory image, to find variables |
| `XBE_SUBST=from=to` | file path substitution |
| `XBE_NOFOG`, `XBE_NOZ`, `XBE_NOCULL`, `XBE_NOALPHATEST` | switch a stage off to isolate a defect |
| `XBE_D3D12_DEBUG=1` | Direct3D 12 debug layer |
