# Third-party software

## whisper.cpp

- Version: v1.9.1
- Commit: `f049fff95a089aa9969deb009cdd4892b3e74916`
- Source: https://github.com/ggml-org/whisper.cpp
- License: MIT
- Build options: CPU is always enabled. Vulkan is enabled by the official Windows release preset. CUDA is available through `SWB_WHISPER_CUDA` and is disabled by default.

The distribution contains the upstream license as `licenses/whisper.cpp-LICENSE.txt`.

The application downloads the multilingual `ggml-base-q5_1.bin` model on demand from the pinned `ggerganov/whisper.cpp` Hugging Face revision. The model is not included in the source tree or release archive. Its manifest records the exact URL, byte size, SHA-256, source, model type and MIT license.

The optional `ggml-silero-v6.2.0.bin` VAD model is downloaded on demand from the pinned `ggml-org/whisper-vad` Hugging Face revision. It is not included in the source tree or release archive. Its manifest records the exact URL, byte size, SHA-256, source and MIT license.

## Dear ImGui

- Source: https://github.com/ocornut/imgui
- License: MIT

The distribution contains the vendored license as `licenses/imgui-LICENSE.txt`.
