<div align="center">
  <img src="assets/icon.png" alt="OptiPiston 图标" width="160">
  <h1>OptiPiston</h1>
  <p><strong>活塞更顺滑，时间可调节。</strong></p>
  <p>面向 Windows LeviLamina 的 Minecraft 基岩版纯客户端模组，提供平滑活塞动画和单人世界变速。</p>

  <p>
    <img src="https://img.shields.io/badge/version-v0.1.0-4c8bf5?style=flat-square" alt="OptiPiston v0.1.0">
    <img src="https://img.shields.io/badge/Minecraft%20Bedrock-Windows%20x64-62b47a?style=flat-square" alt="Windows x64 Minecraft 基岩版">
    <img src="https://img.shields.io/badge/LeviLamina-26.20.*-7b68ee?style=flat-square" alt="LeviLamina 26.20">
    <a href="LICENSE"><img src="https://img.shields.io/badge/license-AGPL--3.0-blue?style=flat-square" alt="AGPL-3.0 许可证"></a>
  </p>

  <p>
    <a href="#快速开始">开始使用</a>
    ·
    <a href="https://github.com/wo55555/OptiPiston/releases">发行版本</a>
    ·
    <a href="CHANGELOG.md">更新日志</a>
    ·
    <a href="https://github.com/wo55555/OptiPiston/issues">问题反馈</a>
    ·
    <a href="README.md">English</a>
  </p>
</div>

> [!WARNING]
> OptiPiston 目前处于早期开发阶段，配置和 API 细节在版本之间仍可能变化。

OptiPiston 按可配置的平滑时间线重新绘制活塞臂及其推动的方块。它只改变客户端的渲染结果，原生活塞进度、方块状态和红石时序都保持不变。在单人世界中，它还能整体加快或放慢世界运行速度，声音和粒子会随之变化。

## 快速开始

> [!IMPORTANT]
> 建议尽量使用干净的 LeviLamina 客户端实例；目前不保证与其他模组广泛兼容。

1. 创建或选择 LeviLamina `26.20.*` 客户端实例。
2. 通过 LeviLauncher/Lip 安装 OptiPiston 的 `#client` 版本，或在实例根目录运行：

   ```powershell
   lip install github.com/wo55555/OptiPiston@0.1.0-mc26.20#client
   ```

3. 启动游戏并进入世界。活塞动画默认开启，可用 `/optipiston status` 查看当前状态。

手动安装：从对应发行版本下载 `OptiPiston-client-windows-x64.zip`，将其中的 `OptiPiston` 目录解压到实例的 `mods` 目录，然后重启客户端。

## 功能

- **平滑活塞动画**：活塞臂和被推方块在 2–4 游戏刻内完成移动，支持小数。仅影响渲染，游戏逻辑保持原生时序。
- **平滑衔接**：被推方块在真实方块完成重新建网格前持续绘制，避免落位时出现空洞、重影或重叠。
- **稳定的活塞头**：每格只绘制一个活塞头；被推动的活塞会随其移动中的机身一起绘制。
- **世界变速**：单人世界以 0.1x–10x 运行，可选同步缩放声音音调和粒子。
- **外部时钟 API**：回放、录制类模组可以通过简单的 C API 驱动动画时钟，使暂停、跳转和倍速保持同步。
- **双语**：命令输出提供英文和简体中文。

## 命令

| 命令                                  | 说明                                      |
| ------------------------------------- | ----------------------------------------- |
| `/optipiston status`                  | 显示活塞动画状态，以及请求/实际世界速度。 |
| `/optipiston piston on`               | 开启活塞动画。                            |
| `/optipiston piston off`              | 关闭活塞动画。                            |
| `/optipiston piston duration <ticks>` | 设置动画时长，范围 `2` 到 `4` 游戏刻。    |
| `/optipiston speed <value>`           | 设置世界速度，范围 `0.1` 到 `10`。        |
| `/optipiston speed next`              | 切换到下一个世界速度预设。                |
| `/optipiston speed prev`              | 切换到上一个世界速度预设。                |
| `/optipiston speed reset`             | 恢复正常速度（`1x`）。                    |

活塞相关设置会写入配置文件。世界变速仅在本地世界、没有其他玩家连接且没有其他模组驱动时钟时生效；未生效时 `/optipiston status` 会显示原因。

## 配置

配置文件位于 `mods/OptiPiston/config/config.json`，通过 Lip 更新时会保留。

```json
{
    "version": 1,
    "locateName": "zh_CN",
    "piston": {
        "enabled": true,
        "durationTicks": 4.0
    },
    "worldSpeed": {
        "presets": [0.1, 0.25, 0.5, 1.0, 2.0, 4.0],
        "audio": true,
        "particles": true
    },
    "command": {
        "enabled": true,
        "command": "optipiston"
    }
}
```

| 键                     | 说明                                     |
| ---------------------- | ---------------------------------------- |
| `piston.enabled`       | 开启或关闭活塞动画。                     |
| `piston.durationTicks` | 动画时长（游戏刻），范围 `2` 到 `4`。    |
| `worldSpeed.presets`   | `speed next` / `speed prev` 循环的速度。 |
| `worldSpeed.audio`     | 声音播放随世界速度缩放。                 |
| `worldSpeed.particles` | 粒子随世界速度缩放。                     |
| `command.command`      | 客户端命令名。                           |

## 模组开发者

OptiPiston 导出名为 `optipiston_get_api` 的 C API。引入 [`include/optipiston/api.hpp`](include/optipiston/api.hpp) 并在运行时查找，OptiPiston 即可保持为可选依赖：

```cpp
#include "optipiston/api.hpp"

if (auto const* api = optipiston::Api::find()) {
    optipiston::ExternalClockLease clock{api};
    // Bump epoch on every jump: seek, clock switch, or export start.
    clock.push(tick, epoch);
    api->set_external_partial(partialTick);
}
```

API v1 还可以读写活塞动画开关和时长；设置值会限制在支持范围内并写入配置。

## 兼容性

| Minecraft / LeviLamina | OptiPiston 版本  | 状态   |
| ---------------------- | ---------------- | ------ |
| `26.20.*`              | `v0.1.0-mc26.20` | 开发中 |

OptiPiston 面向 Windows x64 平台的 Minecraft 基岩版，以纯客户端模组形式发布。暂不支持其他 Minecraft 版本。

## 从源码构建

OptiPiston 使用 xmake、LLVM（clang-cl，CI 使用 LLVM 22）和 Git 在 Windows x64 上构建：

```powershell
xmake f -y -p windows -a x64 -m release --target_type=client --mc=26.20
xmake -y
xmake build optipiston-tests
xmake run optipiston-tests
```

构建产物输出到 `bin/OptiPiston/`。

## 已知限制

- 仅支持 Minecraft 26.20。
- 已在本地世界和 Playback 回放中测试；多人服务器尚未验证。
- 被活塞推动的方块在移动时可能明暗闪烁。原版同样如此，目前尚未修复。
- 世界变速仅限单人，重启后不保留。

如需报告可复现问题，请附带日志和版本信息[创建 Issue](https://github.com/wo55555/OptiPiston/issues)。

## 致谢

特别感谢 [LeviLamina](https://github.com/LiteLDev/LeviLamina) 的维护者与社区提供原生模组开发平台和工具，使 OptiPiston 得以实现。OptiPiston 源自 [Playback](https://github.com/wo55555/Playback) 的活塞渲染工作，核心测试使用 [doctest](https://github.com/doctest/doctest)。

## 许可证

Copyright (C) 2026 [wo555](https://github.com/wo55555)

OptiPiston 采用 [GNU Affero 通用公共许可证 v3.0](LICENSE) 发布。分发修改版本时必须继续使用 AGPL-3.0，并提供对应源代码。第三方组件保留各自许可证。
