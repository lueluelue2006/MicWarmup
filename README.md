# MicWarmup · 麦克风预热

![暖光麦克风插画](Resources/cover.png)

一个轻量的 macOS 菜单栏工具。在豆包输入法开始使用内置麦克风后，短时间请求保持硬件运行，尝试减少下一次语音输入的启动等待。

**[下载编译好的版本](https://github.com/lueluelue2006/MicWarmup/releases)** · [构建与签名](docs/RELEASING.md) · [隐私说明](docs/PRIVACY.md) · [MIT 许可证](LICENSE)

> 当前为实验性预览版。下载包使用固定的自签名证书，尚未经过 Apple 公证。首次打开或更新后可能需要在系统设置中确认；运行效果取决于设备、系统版本和输入法状态。

## 它做什么

- 打开后在菜单栏待命，不显示 Dock 图标。
- 观察到豆包新开始使用内置麦克风后，让硬件保持开启 30 秒。豆包再次开始收音时，从那一刻重新计时 30 秒。
- 也可以手动启动 30 秒预热、提前停止本次预热、关闭自动模式或退出。
- 不注册音频采样回调，不读取、保存或上传声音，也不运行语音模型。

例如，刚开始一次语音输入时，倒计时为 30 秒。如果 20 秒后又开始一次，从第一次开始算，预热一共持续 50 秒。同一条输入流持续活跃或房间里有人说话，都不会自行续时。

这个办法主要帮助**后续录音**。第一次录音仍可能需要等待；预热不能解决网络识别、服务器或系统整体卡顿。硬件保持运行也可能增加耗电，目前没有可靠的额外功耗测量。

## 支持范围

| 项目 | 要求 |
| --- | --- |
| 手动预热 | macOS 12 或更新版本 |
| 自动预热 | macOS 14.2 或更新版本 |
| 麦克风 | 系统能唯一确定的内置输入设备 |
| 输入法 | 系统级安装的豆包输入法，位于 `/Library/Input Methods/DoubaoIme.app` |
| 下载包架构 | Universal，包含 Apple Silicon 和 Intel |

手动模式不依赖豆包输入法。USB、蓝牙麦克风及多个内置输入设备不在当前支持范围。最低版本来自构建目标和 API 要求，不代表所有旧版系统与机器均已实测。

## 安装和使用

1. 从 [Releases](https://github.com/lueluelue2006/MicWarmup/releases) 下载 `.zip`，解压，将 `MicWarmup.app` 拖到「应用程序」或个人「应用程序」文件夹。
2. 打开应用，菜单栏会出现麦克风图标。自动预热默认开启，启动时先待命。
3. macOS 询问麦克风权限时，按你的选择授权。工具虽然不读取声音，保持输入硬件运行仍可能需要这项权限。
4. 照常使用豆包语音输入；在菜单栏查看预热状态和剩余时间。

当前下载包尚未公证。如果系统阻止打开，请先核对下载来源及 Release 中的签名说明，再按 [Apple 的说明](https://support.apple.com/102445) 在「系统设置 → 隐私与安全性」中处理这一个应用的提示。无需关闭系统安全检查或安装根证书。

需要登录后自动打开时，在 macOS 的「系统设置 → 通用 → 登录项」中添加 `MicWarmup.app`；macOS 12 对应「系统偏好设置 → 用户与群组 → 登录项」。构建与安装包不会自行修改登录项。

更新时先从菜单栏退出旧版，再替换同一位置的应用。请持续使用同一发布来源：首次安装、从旧的临时签名切换到固定签名，或更换签名来源时，都可能需要重新授权。固定签名使后续版本可以维持相同的代码身份，但不替代系统权限检查。

## 检测原理

工具订阅 macOS CoreAudio 的进程输入状态和设备变化通知，每五秒做一次健康检查，以便豆包重启后重新绑定。它不监听快捷键，也不依赖豆包提供的插件接口。

只在确认豆包对内置输入设备出现「停用 → 启用」变化时触发。预热期间沿用同一个辅助进程，仅更新截止时间。辅助进程通过 `AudioDeviceStart(device, NULL)` 请求保持硬件运行，不创建音频回调。

系统提供的是输入流状态，所以有这些边界：

- 豆包设置页的麦克风音量检测也可能触发预热。
- 如果两次操作之间输入流始终活跃，工具无法把它们区分成两次开始。
- 已确认使用内置麦克风的短句，按观察到启动的时刻续期。即使界面稍后才处理，倒计时也沿用原始时间。随后确认豆包切到其他输入时，会停止当前自动预热。
- 打开工具、恢复自动模式、唤醒或重新绑定时，先记录当前状态；已经进行的输入不会立即触发。
- 预热期间遇到一次设备查询失败会短暂复查。连续两次失败会停止并显示「设备状态无法确认」。手动停止、关闭自动模式、退出、睡眠、设备确实变化或到期都优先处理。
- 系统音频启动有独立的 15 秒上限。若超时且无法确认麦克风状态，菜单会提示启动超时。若系统未确认正常停止，菜单优先显示「停止未确认」。

## 从源码构建

需要 Apple 命令行开发工具和 macOS SDK 14.2 或更新版本。没有第三方运行时依赖。

```sh
xcode-select --install
git clone https://github.com/lueluelue2006/MicWarmup.git
cd MicWarmup
bash scripts/build.sh
```

构建会先运行模拟测试，再生成应用；这些测试不启动真实麦克风。默认生成当前机器架构、使用临时签名，适合开发验证。

```sh
app=$(bash scripts/build.sh --arch universal)
bash scripts/package.sh "$app"
```

输出在 `build/` 的新目录中，不覆盖已安装的应用。需要稳定授权身份或对外发布时，请参阅 [构建与签名](docs/RELEASING.md)。GitHub Actions 使用临时签名做构建验证；公开 Release 由维护者使用固定证书另行签署。

## 项目与许可

源码和本项目的发布版本采用 [MIT](LICENSE) 许可。封面由 OpenAI 图像生成工具生成，提示词保存在 [Resources/cover-prompt.txt](Resources/cover-prompt.txt)，作为项目资源随同发布，并保留工具写入的 C2PA 来源信息。来源标记及相关商标仍属于各自权利人。

这是独立的社区工具，与豆包及字节跳动没有隶属或官方授权关系。项目不包含豆包的程序、词库、模型或用户输入数据。

问题反馈请说明 macOS 版本、Mac 型号、输入设备类型，以及手动或自动模式的表现。无需提供录音或日常输入内容。

### English summary

MicWarmup is a small macOS menu bar utility that keeps the built-in microphone hardware running for 30 seconds after Doubao IME starts using it. Each new inactive-to-active input transition restarts the 30-second timer. You can also start a 30-second warmup manually. It does not read audio samples, record audio, or send network requests. Automatic mode requires macOS 14.2+, while manual mode targets macOS 12+. Releases are experimental, self-signed, and not yet notarized. Source code and project releases are available under the MIT license.
