# Third party software

- **Xbox XBE Recomp** (TGP482, MIT): `sdk/`, license in `sdk/LICENSE`. The libraries linked into the exe
  (toml++, Dear ImGui, SDL3) and their licenses are listed in `sdk/THIRD_PARTY.txt`, which `release.bat` ships
  beside the exe. The recompiler tool also uses Zydis (MIT), downloaded at configure time.
- **SMAA** (Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, Diego Gutierrez, MIT):
  `src/common/smaa_shader.h` and `src/common/smaa_textures.h`, license in the header of `smaa_shader.h`.
