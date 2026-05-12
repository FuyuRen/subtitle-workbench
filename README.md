# subtitle-workbench

subtitle-workbench是一个Windows字幕工作台。它可以从视频链接或本地视频开始，获取源字幕，调用Whisper兼容接口转录，调用聊天补全接口翻译，并输出双语字幕或硬字幕视频。

程序默认打开图形界面。传入命令行参数时会进入CLI模式，适合批量任务。

## 下载后运行

从GitHub Releases下载`subtitle-workbench-release-windows-x64.zip`，解压到任意目录。

程序运行需要`ffmpeg`、`ffprobe`和`yt-dlp`。发布包默认不包含这些外部工具，推荐用winget安装：

```powershell
winget install --id Gyan.FFmpeg --source winget
winget install --id yt-dlp.yt-dlp --source winget
```

安装完成后重新打开终端或重启程序。也可以把这三个文件放到程序目录下的`vendor\bin`：

```text
subtitle-workbench\
	subtitle-workbench.exe
	config.ini
	vendor\bin\
		ffmpeg.exe
		ffprobe.exe
		yt-dlp.exe
```

复制示例配置，填入自己的接口地址、密钥和模型名：

```powershell
Copy-Item .\config.ini.example .\config.ini
notepad .\config.ini
```

至少需要填写这些字段：

- `whisper_base_url`
- `whisper_api_key`
- `whisper_model`
- `llm_base_url`
- `llm_api_key`
- `llm_model`

双击`subtitle-workbench.exe`即可进入图形界面。选择视频链接或本地视频，确认源语言、目标语言和输出设置后开始任务。生成文件会放在程序目录的`output`文件夹，配置中的`output_dir`可以改成其他目录。

## 命令行使用

命令行入口适合脚本和批量处理。`--source`可以传入视频链接，也可以传入本地视频路径。

```powershell
.\subtitle-workbench.exe run --source "https://www.youtube.com/watch?v=..." --target-lang zh --bilingual
```

已有工作目录可以继续执行：

```powershell
.\subtitle-workbench.exe run --workdir ".\output\video_ab12cd34" --target-lang zh
```

常用参数：

- `--config <path>` 指定配置文件
- `--output-name <name>` 指定输出文件名
- `--output-dir <path>` 指定输出目录
- `--source-lang <lang>` 指定源语言
- `--target-lang <lang>` 指定目标语言
- `--retry-count <n>` 指定失败重试次数
- `--bilingual` 输出双语字幕
- `--no-bilingual` 关闭双语字幕

执行期间，步骤状态会写入标准错误输出；任务结束后，标准输出会给出一段JSON摘要。

## 自己构建发行版

项目当前只支持Windows。构建发行版需要这些工具：

- Visual Studio2022，并安装MSVC C++工具链
- CMake3.25或更高版本
- Ninja

打开“x64 Native Tools Command Prompt for VS2022”或“Developer PowerShell for VS2022”，在源码目录执行：

```powershell
cmake --preset msvc-release
cmake --build --preset release
cpack --config build/release/CPackConfig.cmake -G ZIP -B build/release/dist
```

ZIP会生成在：

```text
build\release\dist\subtitle-workbench-release-windows-x64.zip
```

想做一个自带`ffmpeg`、`ffprobe`和`yt-dlp`的便携包，需要先让这些工具能在当前终端里被找到，再开启捆绑开关：

```powershell
cmake --preset msvc-release -DSWB_BUNDLE_EXTERNAL_TOOLS=ON
cmake --build --preset release
cpack --config build/release/CPackConfig.cmake -G ZIP -B build/release/dist
```

外部工具有各自的发布和许可要求。公开分发自带工具的ZIP前，请确认对应许可文本和再分发条件已经处理妥当。

## 运行测试

```powershell
cmake --preset msvc-debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

测试覆盖配置、音频处理、字幕解析、任务编排、翻译、转录、工作区和CLI。

## 常见问题

提示“未找到ffmpeg”或“未找到yt-dlp.exe”时，检查`ffmpeg.exe`、`ffprobe.exe`、`yt-dlp.exe`是否已经安装到`PATH`，或是否已经放到程序目录的`vendor\bin`。

提示找不到`cl`时，说明当前终端没有载入Visual Studio C++环境。请改用“x64 Native Tools Command Prompt for VS2022”或“Developer PowerShell for VS2022”。

`config.ini`必须和`subtitle-workbench.exe`放在同一个目录，除非命令行里通过`--config`指定了其他路径。