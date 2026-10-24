# ESP32 蓝牙 ↔ Qwen Audio Agent 语音网关固件

基于 au_gw 同一套 ESP32-WROVER-E 硬件（蓝牙 Classic HFP + 以太网 / Wi-Fi），把网络侧从 SIP/RTP 换成
[Qwen Audio Agent](https://github.com/oppsip/qwen-audio-agent) 的 **Gateway Client Protocol**。蓝牙侧角色保持不变。

```
蓝牙耳机 / 手机  ←HFP/A2DP→  ESP32 网关  ←GCP WSS→  audio.zhonglele.top (Nginx)  ←→  qwen-audio-agent Gateway
```

本目录是 ESP-IDF 工程；Gateway 源码在仓库根目录，部署脚本在 `../deploy/`。

## 蓝牙行为（与 au_gw 一致）

| 模式 | 设备 | 何时打开 Qwen 会话 |
| --- | --- | --- |
| **HFP AG（默认）** | 蓝牙耳机 | 耳机连上后自动开会话并打开 SCO；耳机拨号键也可触发；挂断结束 |
| HFP Client | 手机 | 手机来电 RING 时开会话，`voice.ready` 后 ATA 接听，把对端语音交给 Agent |
| A2DP Source | 音箱 | 音箱连上后开会话（仅播放，麦克风 mute） |

音频：SCO 为 CVSD 8 kHz 或 mSBC 16 kHz PCM；固件重采样为 Gateway 宣布的输入速率（通常 16 kHz PCM16LE），经
`input_audio_buffer.append` 上传；下行 `audio.delta`（通常 24 kHz）再采样回 SCO。

## 小程序配置（与 au_gw 同一套小程序）

固件通过 BLE 暴露与网页通道完全相同的 `{ ui, values }` schema，**微信小程序是纯渲染器**，字段、标签、
校验规则全部来自固件，加字段不需要发版。小程序代码只有一份：`oppsip/au_gw` 的
[`miniprogram/`](https://github.com/oppsip/au_gw/tree/main/miniprogram)，用微信开发者工具导入即可，
对 SIP 版（au_gw）与 Qwen 版（本固件）都适用——渲染的标签页由各自固件决定（本固件是 LAN / WiFi / Qwen / 蓝牙 / 高级）。

协议细节见 [`docs/ble_config.md`](docs/ble_config.md)。

1. 按住配置键（GPIO13）上电 3 秒，设备进入 BLE 配置模式，广播 `Qwen-GW-XXXXXX-CFG`。
2. 小程序扫描到后连接，自动向固件取 schema（`get_ui`），展示当前配置。
3. 改完点「保存并重启」：小程序把 `{values}` 一次性提交（`set_config`），固件写 NVS 并自动重启。

## 本机部署 Qwen Audio Agent

需要 Node.js `^22.22.2` 或 `^24.15.0+`，以及百炼 [DashScope API Key](https://bailian.console.aliyun.com/?tab=model#/api-key)。

在**仓库根目录**执行：

```bash
./deploy/start.sh
```

脚本会：

1. 在仓库根目录安装依赖
2. 写入 `~/.config/qwaudio/config.env` 模板（若尚不存在）
3. 生成 `deploy/.env.local`（含 32 字符设备令牌，**不**入库）
4. 启动 Gateway（`0.0.0.0:18888`）和 device-relay（`0.0.0.0:3101`）

把 `DASHSCOPE_API_KEY` 填进 `~/.config/qwaudio/config.env` 后重新执行脚本。前端-only 即可跑通语音（不必装 Backend Agent）。

记下脚本打印的 **电脑局域网 IP** 和 **DEVICE_ACCESS_TOKEN**，写入网关。

本仓库若跑在 **WSL2** 里，ESP32 通常到不了 `172.x` 的 WSL 地址。请在 **Windows 管理员 PowerShell** 里执行：

```powershell
powershell.exe -ExecutionPolicy Bypass -File deploy/wsl-portproxy.ps1
```

脚本会转发 `3101` 和 `18888`。然后把固件里的 Qwen host 填成 **Windows 的 Wi-Fi/有线 IPv4**
（例如 `192.168.1.10`），端口仍是 `3101`；内网其他机器可用 `http://<Windows_IP>:18888` 访问 Gateway。

- 小程序 / BLE 里填：`host`=电脑 LAN IP，`port`=3101，`tok`=令牌
- 或 `idf.py menuconfig` → Device Default Configuration → Qwen Audio Agent

固件连接：

```text
ws://<电脑LAN_IP>:3101/api/realtime
Authorization: Bearer <DEVICE_ACCESS_TOKEN>
```

不要在 ESP32 上填 `127.0.0.1`。WebUI / TUI 与网关不能同时占用同一用户的唯一 Client；测固件前先断开浏览器会话。

## baidu 远端部署

`baidu` 上的服务位于 `/home/lyc/qwen-audio-agent`，使用 Node.js 22.22.2。
`deploy/systemd/` 中的两个服务单元安装在远端的 `~/.config/systemd/user/`，并已设为开机自启。
Gateway 只监听远端 `127.0.0.1:18888`，设备中继监听 `0.0.0.0:3101`，并要求设备令牌。
远端 `deploy/.env.local` 与本地文件使用同一个令牌。

```bash
ssh baidu 'systemctl --user status qwen-audio-gateway qwen-audio-relay'
ssh baidu 'journalctl --user -u qwen-audio-gateway -u qwen-audio-relay -f'
```

访问 WebUI 时，在本机建立 SSH 端口转发，然后打开 `http://127.0.0.1:18889`：

```bash
ssh -N -L 18889:127.0.0.1:18888 baidu
```

ESP32 接入地址为 `ws://120.48.104.98:3101/api/realtime`，端口为 `3101`。
公网 `ws://` 不加密，设备令牌会以明文传输；跨公网使用时应通过受信任的 VPN 或 TLS 代理连接。

> 说明：远端仍沿用旧的目录布局（Gateway 源码在 `~/qwen-audio-agent/repo`，`.env.local` 在
> `~/qwen-audio-agent/deploy`），与本仓库合并后的布局（源码在仓库根，`deploy/` 在仓库内）不同。
> 下次同步远端服务单元时再统一调整，期间不动正在运行的服务。

## audio.zhonglele.top 直连

新 Gateway 通过 Nginx 提供 `wss://audio.zhonglele.top/api/realtime`。固件的 Qwen 配置为：

| 配置项 | 值 |
| --- | --- |
| Host | `audio.zhonglele.top`（不要填 IP） |
| Port | `443` |
| Path | `/api/realtime` |
| TLS | 开启 |
| Token | 新机 `/var/lib/qwaudio/.config/qwaudio/config.env` 中的 `QWEN_AUDIO_GATEWAY_ACCESS_TOKEN` |

旧 relay 的 `DEVICE_ACCESS_TOKEN` 不适用于这个入口。已有 NVS 配置会覆盖 menuconfig 默认值，需通过小程序 / BLE / WebUI 更新上述参数。
WSS 使用 ESP-IDF CA 证书包验证证书链，并校验证书主机名；证书失效、主机名不匹配或设备时钟未同步时连接会失败。
设备启动时会同步 SNTP 时间，日志中的 `cert_flags` 可辅助定位证书错误。

## 编译固件

与 au_gw 相同：ESP-IDF 5.x/6.x，目标 ESP32-WROVER-E（8MB Flash + 8MB PSRAM）。

```bash
cd firmware            # 本目录（仓库根下的 firmware/）
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

首次配网：上电按住配置键进入 BLE 配置（小程序扫码或手填）；或 menuconfig 写入 Wi-Fi / 以太网 / Qwen host。

## 联调建议

1. 仓库根目录 `./deploy/start.sh`，用浏览器打开 `http://127.0.0.1:18888`，或在内网其他机器打开 `http://<电脑LAN_IP>:18888`，确认云端语音可用，然后关掉 WebUI。
2. 烧录固件，配好 LAN IP 和令牌。
3. 耳机配对 `Qwen-GW-XXXXXX`，连上后对着耳机说话，应能听到 Agent 回复。

若串口日志显示 `esp_ws_handshake_status_code=401`，说明设备中继拒绝了 WebSocket 握手。
确认 Qwen 路径为 `/api/realtime`，并把令牌设为与中继 `DEVICE_ACCESS_TOKEN` 完全相同的值（小程序 Qwen 页的「设备令牌」字段）。
`firmware/sdkconfig` 中的令牌是出厂默认值；设备已保存的 NVS 配置会覆盖它。改完固件默认值后仍报 401 时，请检查 NVS 中的令牌。
