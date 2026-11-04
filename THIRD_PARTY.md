# Third party software

- **Xbox XBE Recomp** (TGP482, MIT): `sdk/`, license in `sdk/LICENSE`. The libraries linked into the exe
  (toml++, Dear ImGui, SDL3) and their licenses are listed in `sdk/THIRD_PARTY.txt`, which `release.bat` ships
  beside the exe. The recompiler tool also uses Zydis (MIT), downloaded at configure time.
- **SMAA** (Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, Diego Gutierrez, MIT):
  `src/common/smaa_shader.h` and `src/common/smaa_textures.h`, license in the header of `smaa_shader.h`.
- **Conker: Live & Recompiled** (cdetv, MIT, github.com/cdetv/conker-live-recompiled): the addresses and the
  approach of the fixes in `src/game/cdetv*.cpp` / `cdetv.h` and their `[[hook]]` entries in `config/game.toml`
  come from its `src/recomp_manual.c`; the code is rewritten for this runtime. Nothing of what its licence leaves
  out (the translated game code, `xemu_apu.dll`) is used. Its licence:

      MIT License

      Copyright (c) 2026 cdetv

      Permission is hereby granted, free of charge, to any person obtaining a copy
      of this software and associated documentation files (the "Software"), to deal
      in the Software without restriction, including without limitation the rights
      to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
      copies of the Software, and to permit persons to whom the Software is
      furnished to do so, subject to the following conditions:

      The above copyright notice and this permission notice shall be included in all
      copies or substantial portions of the Software.

      THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
      IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
      FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
      AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
      LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
      OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
      SOFTWARE.
