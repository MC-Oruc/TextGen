# TextGen

[![Unreal Engine 5.7](https://img.shields.io/badge/Unreal%20Engine-5.7-blue.svg)](https://www.unrealengine.com/)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

Reusable Unreal Engine text-generation plugin with KoboldCpp, OpenAI, and llama.cpp providers.

Managed llama.cpp support targets Win64 x64 with CUDA 13.3, CUDA 12.4, and Vulkan runtimes. KoboldCpp and OpenAI remain external providers and are never owned or terminated by this plugin.

No llama.cpp executable or accelerator DLL is stored in this repository. After the plugin is enabled and the Editor restarts, TextGen automatically prepares the configured official CUDA 13.3, CUDA 12.4, and Vulkan runtimes under the project `Saved` directory. Downloads stream directly to disk, reject unsafe archive paths, and publish atomically.

The plugin depends on the sibling `ProcessRuntime` plugin for generic owned-process lifecycle management.

## Installation

Add both plugins as project submodules, enable `TextGen` in the project
descriptor, and build the Editor target:

```powershell
git submodule add https://github.com/MC-Oruc/ProcessRuntime.git Plugins/ProcessRuntime
git submodule add https://github.com/MC-Oruc/TextGen.git Plugins/TextGen
git submodule update --init --recursive
```

## GGUF and cache Content Browser integration

When the plugin is enabled in the editor, configured `.gguf` files and active Default `.bin` cache files under Unreal Content directories are visible in their existing `/Game/...` Content Browser folders. The integration does not create a synthetic browser root or expose Saved runtime Progress files.

Reveal/edit and preview open the file in Explorer. Delete uses the operating-system deletion flow; deleting a configured GGUF clears matching project-default model references.

Project model ownership stays in `Config/DefaultGame.ini`; the plugin defaults leave `ModelPath` empty. Packaged Win64 builds stage the validated `PackagedDefaults.ModelPath` GGUF and every active `*_Default.bin` under `DefaultCacheDirectory` as loose NonUFS files. Saved Progress caches are never staged.

## Managed llama.cpp contract

- Managed binaries are downloaded Win64 x64 accelerator builds. The default is CUDA 13.3; CUDA 12.4 and Vulkan are selectable alternatives. Each accelerator build can execute the mandatory CPU safety mode without a separate CPU archive.
- Backend capability is probed from `llama-server --list-devices`. Auto placement starts with dense tensors and the shared expert on the GPU while routed sparse experts remain on the CPU. A device below the 4 GB class starts in CPU mode. Memory-allocation failure degrades monotonically to dense-only GPU and then full CPU, and the safe result is remembered for the same model, device, runtime, context, and KV configuration.
- TextGen launches the selected installed `llama-server.exe` as a model-less router bound to `127.0.0.1`. llama.cpp API keys, external llama URLs and raw process arguments are not supported.
- Editor lifecycle is `EditorSession`, `PIESession` or `Manual`. Packaged games reconcile only after the persisted active profile is loaded, so llama.cpp starts only when the active provider is llama.cpp and managed runtime is enabled.
- Full process output is written through `LogTextGenAPI`; the service retains the latest 200 lines for UI diagnostics.
- KV `.bin` files are performance caches. Canonical role/content history remains owned and saved by the game conversation layer.
- Gemma 4 persistent slot-cache reuse requires `--swa-full`. This does not enable SWA; it allocates a full-size SWA KV cache so a restored `.bin` prefix can be reused. The upstream Gemma 4 bug was fixed by [llama.cpp #22288](https://github.com/ggml-org/llama.cpp/pull/22288), which closed [#21468](https://github.com/ggml-org/llama.cpp/issues/21468).
- A controlled A/B test must change only `--swa-full`. With F16 K/V, one slot and a 4096-token context, the local Gemma 4 E2B test measured about 32 MiB additional peak working set. The restored prefix reduced warm processing from 826 to 23 tokens and prompt evaluation from 11.49 s to 0.47 s. Results scale with context size, K/V type and parallel slot count; run `Scripts/Test-LlamacppSlotPerformance.ps1` on target hardware for current values.
- Model and model-setting changes use `/models/unload` then `/models/load` without replacing the parent process. Port, runtime tag and process failure are parent-restart boundaries.
- Each NPC/day key has one immutable project-owned Default cache and one Saved Progress cache. Missing or incompatible caches fall back to canonical messages without failing the conversation.

Install the latest official runtime:

```powershell
pwsh -File ./Scripts/Install-Llamacpp.ps1 -Backend CUDA13
```

Install an exact stable or nightly tag. Stable `vX.Y.Z` releases are resolved through their
official `nightly-tag.txt` pointer while the installed runtime keeps the stable tag as its identity:

```powershell
pwsh -File ./Scripts/Install-Llamacpp.ps1 -Tag v0.3.0 -Backend CUDA12
```

Install an exact nightly/dev tag through the same API:

```powershell
pwsh -File ./Scripts/Install-Llamacpp.ps1 -Tag b10621 -Backend CUDA12
```

Update the separately cached CUDA dependency package only when required:

```powershell
pwsh -File ./Scripts/Install-Llamacpp.ps1 -Tag v0.3.0 -Backend CUDA13 -Component CudaDependencies
```

The installer writes to `Saved/TextGen/Runtimes/Llamacpp/Win64/<backend>/<tag>`. It accepts only the selected official accelerator asset, rejects unsafe archive paths, and retains installed tags. CUDA dependencies are cached independently so routine runtime updates do not download them again.

Packaged Win64 builds require all three verified runtimes for the configured exact tag and stage them as loose NonUFS files beside the game executable. Players receive a ready-to-run CUDA 13, CUDA 12, and Vulkan distribution without a manual runtime installation. Settings UI updates still publish to the writable `Saved` directory, which takes precedence over the shipped copy without modifying the installed game files.
