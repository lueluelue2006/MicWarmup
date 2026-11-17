# 构建、稳定签名与发布

## 三种签名模式

| 模式 | 用途 | 跨版本身份与分发 |
| --- | --- | --- |
| `adhoc` | 本地开发、CI 验证 | 身份随代码变化，更新可能重新询问权限；未公证 |
| `local` | 使用固定证书的预览发布或个人构建 | 持续使用同一证书和标识符以维持代码身份；不等于 Apple Developer ID，未公证 |
| `developer-id` | 正式对外分发 | 使用 Apple Developer ID Application，安全时间戳和 Hardened Runtime；随后提交公证 |

本项目当前的公开预览包采用 `local` 模式。证书及私钥保留在维护者的钥匙串中，不随源码或 CI 配置发布。构建脚本不会创建证书、修改钥匙串信任设置或重置系统隐私权限。

macOS 使用代码签名要求来识别已授权的程序。稳定签名可让更新继续符合旧授权的身份要求；首次授权仍需用户操作。更换证书、改变签名要求或使用其他来源的构建，可能再次触发授权。参阅 [Apple TN3127](https://developer.apple.com/documentation/technotes/tn3127-inside-code-signing-requirements/)。

## 本地构建

```sh
bash scripts/build.sh --arch arm64
bash scripts/build.sh --arch x86_64
bash scripts/build.sh --arch universal
```

模拟测试在构建机器的本机架构上运行。Universal 构建分别编译两种架构再合并，因此主程序与辅助进程都包含两种架构。交叉编译成功不代表已在另一种硬件或全部旧版 macOS 上验证。

## 配置固定签名

在钥匙串中准备你有权使用的代码签名证书及其私钥。可以通过以下只读命令查找身份：

```sh
security find-identity -v -p codesigning
```

建立被 Git 忽略的 `.local/signing.env`，填写自己的证书 SHA-1 指纹：

```text
MW_SIGNING_MODE=local
MW_SIGNING_IDENTITY=YOUR_40_HEX_DIGIT_CERTIFICATE_FINGERPRINT
```

也可以通过构建参数或同名环境变量设置。签名身份不存在或不匹配时，构建会失败，不会静默改成临时签名。钥匙串可能使用原生弹窗询问私钥访问权限。

```sh
app=$(bash scripts/build.sh --arch universal)
zip=$(bash scripts/package.sh "$app")
```

签名先处理辅助进程，再处理整个应用。包名注明签名和公证状态，ZIP 中仅包含 `MicWarmup.app`，旁边另有 SHA-256 校验文件。

固定保留应用标识符 `local.doubaolab.MicWarmup` 和辅助进程标识符 `local.doubaolab.MicWarmup.Helper`。这是早期版本沿用的标识符，不表示需要实验项目才能运行。

## Apple Developer ID 与公证

Apple 公证要求有效的 Developer ID 签名、Hardened Runtime 及安全时间戳。自签名和 Apple Development 证书均不能代替 Developer ID。参阅 [Apple 公证要求](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution)。

配置 `MW_SIGNING_MODE=developer-id` 和对应证书后构建。应用及辅助进程只申请保持输入硬件所需的 audio-input 权限，不启用调试或取消库验证等例外。

公证凭据应提前存放在 Apple `notarytool` 的钥匙串 profile 中，不能提交到 Git。以下脚本会上传编译包到 Apple，检查接受状态，并在新的应用副本上装订公证票据。完成后才可生成 `notarized` 标记的发布包。

```sh
notarized_app=$(bash scripts/notarize.sh --profile YOUR_KEYCHAIN_PROFILE "$app")
bash scripts/package.sh --notarized "$notarized_app"
```

从自签名切换为 Developer ID 是签名身份迁移，可能需要用户再授权一次。不要承诺所有构建来源都能共用同一份权限记录。

## GitHub Releases

1. 更新 `Support/Info.plist` 的版本号和 build 号，并更新 `CHANGELOG.md`。
2. 提交源码；检查 GitHub Actions 的 Apple Silicon 与 Intel 构建结果。
3. 从待发布提交构建 Universal 应用并使用固定证书签名，检查签名和权限。正式包完成 Apple 公证。
4. 打包应用，为对应提交建立版本标签，在 Releases 上传 ZIP、SHA-256 校验文件及 `LICENSE`。
5. 发布说明写明架构、最低系统版本、签名、公证状态以及已知限制。未经完整实机验证的版本标记为预览版。

GitHub 会在对应标签下提供源码 ZIP/TAR。不要将本机 `.local/`、`build/`、签名私钥或实验数据提交到仓库。CI 的临时签名产物仅用于验证，不替代维护者的固定签名发布包。
