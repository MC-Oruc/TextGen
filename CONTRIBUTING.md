# Contributing

TextGen targets Unreal Engine 5.7 and keeps provider, process, editor, and project-policy boundaries explicit.

## Development

1. Clone the host project with the `TextGen` and `ProcessRuntime` submodules.
2. Keep llama.cpp executables and accelerator DLLs outside source control. Managed payloads belong under the host project's `Saved/TextGen` directory.
3. Build and test the plugin with unity and precompiled headers disabled before submitting changes.
4. Keep commits focused and describe behavior, validation, and compatibility impact.

## Pull requests

- Preserve the reusable plugin boundary; TextGen must not depend on project gameplay modules.
- Add or update automation coverage for public behavior changes.
- Do not add generated files, model files, runtime binaries, credentials, or local caches.
- Report the exact Unreal target and validation commands used.
