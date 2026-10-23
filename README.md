# Conker: Live & Reloaded — XBE Recomp

An unofficial native Windows port of **Conker: Live & Reloaded** (original Xbox, 2005), made by static
recompilation of the game's `default.xbe`. No emulator: the game's x86 code is translated to C++ and compiled
for x64, and the Xbox kernel, Direct3D 8, DirectSound and XAPI are reimplemented on Windows (Direct3D 12,
XAudio2, SDL3).

*Version française plus bas.*

> [!IMPORTANT]
> This repository contains **no game data, no XBE and no recompiled code**. You need your own legally obtained
> copy of the game. Only the **European release** is supported (see [Supported version](#supported-version)).

## Status

Work in progress, but the single-player game is playable:

- Intro videos, menus, profiles and saves, cutscenes, music, sound effects and voices work.
- 31 levels of the adventure have been loaded and checked without crashes,
  running at 60 fps on an RTX 3060 at 1080p with 150 % supersampling
  (BeachRunup, the heaviest level with 13,000 draws per frame, at 57-59 fps).
- Xbox controller fully mapped (analog buttons, triggers, White/Black on LB/RB, rumble).
- Game text in French, German, Spanish, Italian, Japanese or English (from the European disc).

Known issues:

- Conker's tail can look paler than on the console.
- Multiplayer: the menus, game creation and lobby work; matches are untested. Xbox Live no longer exists.
- At the start of *Hungover* Conker looks blurred and doubled: that is the game's own hangover effect.

Detailed progress notes: [docs/TECHNICAL_NOTES.md](docs/TECHNICAL_NOTES.md).

## Features

- Any resolution, borderless, fullscreen or windowed; 16:9 and wider.
- Internal resolution up to 200 % (supersampling), SMAA, adaptive sharpening, 16x anisotropic filtering.
- Unlocked frame rate (60 fps by default, or just under the monitor's refresh rate), vsync.
- Modern additions, all optional: soft shadows, larger shadow maps, ambient occlusion (GTAO).
- The console's audio DSP effects (I3DL2 reverb, filters) reproduced.
- In-game options overlay on **F1** (mouse), saved to `settings.toml`. F7 / F8 / F9 toggle SMAA, ambient
  occlusion and soft shadows for comparison.

## Supported version

Conker: Live & Reloaded, **Europe** (En, Ja, Fr, De, Es, It), title ID `4D530051`.
`default.xbe` SHA-1: `68a37f0d03d73dabc86a848df5ec1820dd551254`.

The function and data addresses in `config/game.toml` belong to this executable. Other releases (USA) would need
their own symbols (`xbe_symbols`, then the hand-written `[abi]`, `[data]` and hook addresses).

## Building

1. Install Visual Studio 2022 or later with **Desktop development with C++**, **C++ Clang tools for Windows**
   and **C++ CMake tools for Windows** (`build.bat` offers to add the last two).
2. Clone this repository with [Git](https://git-scm.com/).
3. Put your game files in `assets`: `default.xbe` with the `dvddata` and `media` folders beside it (extract your
   disc image, for instance with [extract-xiso](https://github.com/XboxDev/extract-xiso)).
4. Run `build.bat`. The first build recompiles the game (4.7 MB of x86 code) and takes a long time; the exe is
   written to `out\build\release\Conker.exe`.

## Playing

Put an `assets` folder beside `Conker.exe` holding either your disc image (`.iso`) or the extracted files, then
run `Conker.exe`. Saves go to the `hdd` folder beside the exe.

`settings.toml` beside the exe (all optional):

| Setting | Default | Meaning |
|---|---|---|
| `display_mode` | `"borderless"` | `"borderless"`, `"fullscreen"` or `"windowed"` |
| `width`, `height` | `0` | output resolution, 0 = desktop |
| `render_percent` | `150` | internal resolution in % of the output height (25 to 200) |
| `fps_cap` | `60` | 0 = unlimited, 1 = just under the monitor's refresh rate |
| `vsync` | `true` | |
| `anisotropic_filtering` | `16` | 0 = the game's own filtering |
| `langue` | `"francais"` | game text: `francais`, `anglais`, `allemand`, `espagnol`, `italien`, `japonais` |
| `smaa` | `true` | SMAA anti-aliasing |
| `nettete` | `40` | sharpening in %, 0 = off |
| `ombres_douces`, `ombres_taille` | `true`, `1024` | soft shadows, shadow map size (0 = the game's) |
| `occlusion_ambiante`, `occlusion_rayon`, `occlusion_force` | `true`, `40`, `100` | ambient occlusion, radius, strength % |

Voices exist only in English on the disc; the game subtitles them in the chosen language.

## Contributing

Bug reports with the level, what you did and a screenshot are welcome (use the issue template). Logs are in
`logs\conker.log` beside the exe. Never attach game files, XBE dumps or recompiled code.

## Credits

- [Xbox XBE Recomp](https://github.com/TGP482/XboxXBERecomp) by TGP482: the recompiler and runtime this port
  is built on (copied in `sdk/`, local changes tracked; upstream commit in `sdk/UPSTREAM_COMMIT`).
- [SMAA](https://github.com/iryoku/smaa), [Zydis](https://github.com/zyantific/zydis),
  [toml++](https://github.com/marzer/tomlplusplus), [Dear ImGui](https://github.com/ocornut/imgui),
  [SDL](https://github.com/libsdl-org/SDL): see [THIRD_PARTY.md](THIRD_PARTY.md).
- Conker: Live & Reloaded is © Microsoft / Rare. This project is not affiliated with or endorsed by Microsoft,
  Rare or Xbox.

## License

The code of this repository is under the [MIT License](LICENSE). `sdk/` keeps its own MIT license.

---

# Version française

Portage PC natif et non officiel de **Conker: Live & Reloaded** (Xbox, 2005), par recompilation statique du
`default.xbe` du jeu : pas d'émulateur, le code x86 du jeu est traduit en C++ et compilé pour x64 ; le noyau
Xbox, Direct3D 8, DirectSound et XAPI sont réécrits pour Windows (Direct3D 12, XAudio2, SDL3).

> [!IMPORTANT]
> Ce dépôt ne contient **aucune donnée du jeu, ni XBE, ni code recompilé**. Il faut votre propre copie du jeu,
> en **version européenne** (SHA-1 du `default.xbe` : `68a37f0d03d73dabc86a848df5ec1820dd551254`).

**État** : en cours, l'aventure est jouable. Vidéos, menus, sauvegardes, cinématiques, musique et voix
fonctionnent ; 31 niveaux vérifiés sans plantage, 60 images/s en 1080p ; manette Xbox complète ; textes en
français (ou autre langue du disque européen). Défauts connus : queue de Conker parfois trop
pâle, parties multijoueur non testées (menus et salon fonctionnent). Au début de *Hungover*, Conker flou et
dédoublé : c'est l'effet de gueule de bois prévu par le jeu.

**Compiler** : Visual Studio 2022 ou plus récent (C++, outils Clang et CMake), placer `default.xbe` et les
dossiers `dvddata` et `media` dans `assets`, lancer `build.bat`. La première compilation est longue.

**Jouer** : un dossier `assets` à côté de `Conker.exe` avec votre image disque (`.iso`) ou les fichiers
extraits. Réglages dans `settings.toml` (tableau ci-dessus) ou en jeu avec **F1**.

Licence MIT. Conker: Live & Reloaded © Microsoft / Rare ; projet sans lien avec Microsoft, Rare ou Xbox.
