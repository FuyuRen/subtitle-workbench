# subtitle-workbench

subtitle-workbench是一款Windows字幕工具，可处理视频链接和本地视频，生成字幕译文或带硬字幕的视频。语音识别可选本地whisper.cpp或Whisper兼容API。

## 使用

### 安装依赖

程序需要`ffmpeg`、`ffprobe`和`yt-dlp`。可以通过winget安装：

```powershell
winget install --id Gyan.FFmpeg --source winget
winget install --id yt-dlp.yt-dlp --source winget
```

程序会从系统`PATH`和程序目录下的`vendor\bin`查找这些工具。

### 配置

复制示例配置：

```powershell
Copy-Item .\config.ini.example .\config.ini
notepad .\config.ini
```

本地语音识别是默认选项。进入设置页下载语音模型，再选择计算方式、GPU设备、线程数和VAD。

Whisper API模式需要填写以下配置：

- `whisper_base_url`
- `whisper_api_key`
- `whisper_model`

翻译功能需要填写以下配置：

- `llm_base_url`
- `llm_api_key`
- `llm_model`

`config.ini`通常放在`subtitle-workbench.exe`所在目录。命令行可以通过`--config`指定其他位置。

### 图形界面

双击`subtitle-workbench.exe`打开程序。

1. 在设置页选择本地语音识别或Whisper API。
2. 选择视频文件，或粘贴视频链接。
3. 设置源语言、目标语言、字幕样式和输出目录。
4. 点击开始，生成结果会写入`output_dir`指定的目录。该项为空时使用程序目录下的`output`。

本地模型可以在设置页下载、重新下载或删除。下载完成后，本地语音识别可以离线运行。

### 命令行

处理视频链接或本地文件：

```powershell
.\subtitle-workbench.exe run --source "https://www.youtube.com/watch?v=..." --target-lang zh --bilingual
.\subtitle-workbench.exe run --source ".\video.mp4" --source-lang en --target-lang zh
```

继续已有工作目录：

```powershell
.\subtitle-workbench.exe run --workdir ".\output\video_ab12cd34" --target-lang zh
```

常用参数：

- `--config <path>`指定配置文件
- `--output-name <name>`指定输出文件名
- `--output-dir <path>`指定输出目录
- `--source-lang <lang>`指定源语言
- `--target-lang <lang>`指定目标语言
- `--retry-count <n>`指定重试次数
- `--bilingual`生成双语字幕

## 构建

### 构建环境

- Visual Studio 2022及MSVC C++工具链
- CMake 3.25或更高版本
- Ninja

Vulkan发行版还需要LunarG Vulkan SDK 1.4.350.0。CUDA构建还需要CUDA Toolkit。配置阶段会下载固定版本的whisper.cpp，请保持网络连接。

请在“x64 Native Tools Command Prompt for VS2022”或“Developer PowerShell for VS2022”中执行命令。

### Vulkan发行版

```powershell
cmake --preset msvc-release
cmake --build --preset release
cpack --config build/release/CPackConfig.cmake -G ZIP -B build/release/dist
```

生成的安装包位于：

```text
build\release\dist\subtitle-workbench-release-windows-x64.zip
```

### CPU构建

```powershell
cmake -S . -B build/cpu -G Ninja -DCMAKE_BUILD_TYPE=Release -DSWB_WHISPER_VULKAN=OFF -DSWB_WHISPER_CUDA=OFF
cmake --build build/cpu
```

### CUDA构建

```powershell
cmake -S . -B build/cuda -G Ninja -DCMAKE_BUILD_TYPE=Release -DSWB_WHISPER_VULKAN=OFF -DSWB_WHISPER_CUDA=ON
cmake --build build/cuda
```

### 运行测试

```powershell
cmake --preset msvc-debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```
