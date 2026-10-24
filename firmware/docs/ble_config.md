# BLE 配置协议（固件 ⇄ 小程序）

与 SIP 版（au_gw）**同一份协议**，本固件只是 schema 里的「SIP」标签页换成「Qwen」标签页。
小程序代码只有一份（`oppsip/au_gw` 的 `miniprogram/`），它只负责**渲染、修改、提交**，配置内容全部由固件提供。
整条链路不依赖网络、平台、账号、备案域名：设备上电时按住 GPIO13 3 秒进入 BLE 配置模式，小程序连上后
向固件要 schema，改完一次性提交，固件写入 NVS 并自行重启。

## 进入配置模式

```
上电 → NVS → 采样 GPIO13（3 秒）
     → 按住：直接进 BLE 配置模式（与网络状态无关）
     → 未按住：以太网/WiFi → 网络可用走 Web 会话，不可用才回退 BLE
```

BLE 配置模式广播名 `<蓝牙名>-CFG`（如 `Qwen-GW-B3C4D5-CFG`）。小程序按标签上的 MAC 后 6 位
过滤，仅一台匹配时自动连接。

> 该模式下 Classic 蓝牙不启动（Classic 扫描与 BLE GATT 互斥），所以 `picker` 拿不到附近设备列表，
> 只能手填名称。这是 BLE 通道相对网页通道唯一的能力缺口。

## GATT 通道

| UUID | 方向 | 说明 |
|------|------|------|
| `FFF0` | 服务 | |
| `FFF1` | 手机 → 设备（write） | JSON + `\n`，按 MTU 分片 |
| `FFF2` | 设备 → 手机（notify） | JSON + `\n`，按 MTU 分片 |

- 协商 MTU 200，每片最多 197 字节（ATT 开销 3），片间 20 ms。
- 一条消息以 **单独的 `\n` 帧**结束；接收方必须按 `\n` 重组，不能假设一帧一条消息。
- 报文体是 **UTF-8**（固件返回的 `label.zh` 就是中文）。按字节分片，一个汉字的 3 个字节可能
  跨两帧，因此**只能按字节累积、凑齐一整行后再解码**，不能逐帧解码、也不能按字符切片。
- 小程序侧组装缓冲上限 32 KB；固件侧接收缓冲 4 KB（`BLE_CFG_RX_BUF_SIZE`，够放下整个 `{values}` 对象）。
- `get_ui` 的响应约 4–5 KB（约 25 帧、秒级），因此固件用独立任务发送，避免阻塞 BTC 任务。

## 命令

### `get_ui` — 取 schema + 当前值

请求 `{"cmd":"get_ui"}`，响应：

```json
{
  "ok": true,
  "fw": "1.2.3",
  "ui": {
    "tabs": [
      { "id": "lan",  "label": { "zh": "有线网络", "en": "Ethernet" }, "fields": [ /* edh/eip/emk/egw/edn */ ] },
      { "id": "wifi", "label": { "zh": "WiFi",     "en": "WiFi"     }, "fields": [ /* ssid/wpw */ ] },
      { "id": "qwen", "label": { "zh": "Qwen",     "en": "Qwen"     }, "fields": [
        { "key": "host", "type": "text",     "label": { "zh": "Gateway / Relay 主机", "en": "Gateway / Relay Host" } },
        { "key": "pt",   "type": "number",   "min": 1, "max": 65535 },
        { "key": "path", "type": "text" },
        { "key": "tok",  "type": "password", "placeholder": "留空则保持原令牌 / leave unchanged" },
        { "key": "tls",  "type": "switch" },
        { "key": "auto", "type": "switch" }
      ] },
      { "id": "bt",   "label": { "zh": "蓝牙", "en": "Bluetooth" }, "fields": [ /* btn/btm/bta/btr/wle/wlc */ ] },
      { "id": "adv",  "label": { "zh": "高级", "en": "Advanced"  }, "fields": [ /* frs */ ] }
    ]
  },
  "values": { "edh": 1, "ssid": "", "wpw": "********", "host": "192.168.1.10",
              "pt": 3101, "path": "/api/realtime", "tok": "********",
              "tls": 0, "auto": 1, "btm": 0, "wle": 0 },
  "contacts": { "enabled": false, "items": [] }
}
```

### `set_config` — 一次性提交

请求：

```json
{ "cmd": "set_config", "values": { "ssid": "MyWiFi", "wpw": "secret", "host": "192.168.1.10" } }
```

- 键与 `get_ui` 返回的 `values` 相同；**缺失的键保持设备原值**，所以可以只发改动的字段。
- 密码/令牌保持 `********` 或传 `""` 都表示「不改」。`tok` 为空且 `host` 为空时 Qwen 配置不变。
- `{"frs": 1}` 表示恢复出厂（清除网络/Qwen/蓝牙配对，保留平台注册）。
- 应用成功：回 `{"ok":true}`，300 ms 后设备自动重启。
- 失败：回 `{"ok":false,"err":"ESP_ERR_INVALID_ARG"}` 之类的错误名，设备不重启。

### `get_contacts` — 只读通讯录

请求 `{"cmd":"get_contacts"}`，响应 `{"ok":true,"contacts":{...}}`。用在 `roster` 字段上，
不必重新拉整个 schema。

### 兼容的底层命令

`get_config` / `set_eth` / `set_wifi` / `set_qwen`（别名 `set_sip`）/ `set_bt` / `factory_reset` / `reboot`
保留给产线与旧工具。新代码请用 `get_ui` + `set_config`。

## 渲染约定（小程序 = 渲染器）

`ui.tabs[].fields[]` 的 `type` 取值与含义：

| type | 控件 | 关键属性 |
|------|------|----------|
| `text` | 单行输入 | `maxlength` `placeholder` |
| `password` | 密码输入（可切换明文） | `maxlength` `placeholder`；值为 `********` 时聚焦即清空 |
| `number` | 数字输入 | `min` `max` |
| `select` | 下拉选择 | `options[]` |
| `radio` | 单选按钮组 | `options[]` |
| `check` | 多选（值为数组） | `options[]` |
| `switch` | 开关（值为 0/1） | |
| `picker` | 设备列表 + 手动输入 | `empty` `unavailable` |
| `action` | 危险操作按钮 | `button` `confirm` |
| `roster` | 只读列表 | `empty` |

通用属性：`label.{zh,en}`、`hint.{zh,en}`、`visible_when`、`options[].{value,label}`、`min`/`max`/`maxlength`。

- `visible_when`：`{ key: value }` 或 `{ key: [v1, v2] }`，全部匹配才显示；值变化后需重新计算。
- `label`/`hint`/`options` 都能是多语言对象，小程序按当前语言取值。
- 提交时：`********` 一律转成 `""`；`roster` 与 `action` 字段不进入 `values`（`action` 单独提交
  `{frs:1}`）。

## 与网页通道的差异

| | BLE（小程序） | Web 会话 |
|---|---|---|
| 依赖网络/账号/HTTPS | 不需要 | 需要 |
| 字段来源 | 固件 `get_ui` | 固件 `PUT session` 的 `ui` |
| 提交 | `set_config`（原子） | `POST /me/session` |
| 附近蓝牙列表 | 无（互斥）→ 手填 | 有（2.5 s 轮询 `scan`） |
| 会话草稿 `draft` | 不需要 | 切换 `btm` 时同步扫描角色 |
| 并发配置 | 单连接 | 可多端 |
| 重启 | 提交成功后固件自动重启 | 设备应用后重启 |

## 固件侧实现位置

- `main/web_config.c` / `web_config.h`：`web_config_build_ui()` / `_build_values()` /
  `_build_contacts()` / `web_config_apply_values()` / `web_config_json_init()` 由网页通道复用为公共接口。
- `main/ble_config_server.c`：`get_ui` / `set_config` / `get_contacts`；接收缓冲 4096；
  大响应由独立任务 `send_notify_owned()` 发送。
