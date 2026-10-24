#!/usr/bin/env bash
# Install and run qwen-audio-agent Gateway + LAN device-relay on this machine.
set -euo pipefail

# 仓库根目录即 qwen-audio-agent 源码（firmware/ 与 deploy/ 均在仓库内）。
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$ROOT"
DEPLOY="$ROOT/deploy"
ENV_LOCAL="$DEPLOY/.env.local"
CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/qwaudio"
CONFIG_ENV="$CONFIG_DIR/config.env"

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "缺少命令：$1" >&2
    exit 1
  }
}

NODE_BIN="$(command -v node)"
if [[ -x "$HOME/.nvm/versions/node/v22.22.2/bin/node" ]]; then
  NODE_BIN="$HOME/.nvm/versions/node/v22.22.2/bin/node"
  export PATH="$HOME/.nvm/versions/node/v22.22.2/bin:$PATH"
elif [[ -s "$HOME/.nvm/nvm.sh" ]]; then
  # shellcheck disable=SC1091
  source "$HOME/.nvm/nvm.sh"
  nvm use 22.22.2 >/dev/null
  hash -r
  NODE_BIN="$(command -v node)"
fi
need_cmd node
need_cmd npm
echo "Node $($NODE_BIN -v), npm $(npm -v)"
NODE_MAJOR="$($NODE_BIN -p 'process.versions.node.split(".")[0]')"
if [[ "$NODE_MAJOR" -lt 22 ]]; then
  echo "需要 Node.js 22.22.2+ 或 24.15.0+" >&2
  exit 1
fi

echo "==> 安装 qwen-audio-agent 依赖（repo/）"
cd "$REPO"
if [[ ! -d node_modules ]]; then
  npm install --engine-strict=false
else
  echo "node_modules 已存在，跳过 npm install（删除后可强制重装）"
fi

mkdir -p "$CONFIG_DIR"
if [[ ! -f "$CONFIG_ENV" ]]; then
  umask 077
  cat > "$CONFIG_ENV" <<'EOF'
# Fill in a Bailian / DashScope key: https://bailian.console.aliyun.com/?tab=model#/api-key
DASHSCOPE_API_KEY=
QWEN_AUDIO_REALTIME_MODEL=qwen-audio-3.0-realtime-plus
AGENT_PROTOCOL=none
EOF
  echo "已创建配置模板：$CONFIG_ENV"
fi

if ! grep -q '^DASHSCOPE_API_KEY=.\+' "$CONFIG_ENV"; then
  echo
  echo "请先编辑 $CONFIG_ENV 填入 DASHSCOPE_API_KEY，然后重新运行本脚本。"
  echo "申请地址：https://bailian.console.aliyun.com/?tab=model#/api-key"
  echo "没有 Key 时 Gateway 会拒绝启动。"
  exit 1
fi

if [[ ! -f "$ENV_LOCAL" ]]; then
  TOKEN="$(node -e 'console.log(require("crypto").randomBytes(24).toString("base64url"))')"
  cat > "$ENV_LOCAL" <<EOF
DEVICE_ACCESS_TOKEN=${TOKEN}
GATEWAY_URL=http://127.0.0.1:18888
DEVICE_HOST=0.0.0.0
DEVICE_PORT=3101
DEVICE_ALLOW_TOKEN_FREE=0
EOF
  echo "已生成 $ENV_LOCAL"
fi

# shellcheck disable=SC1090
set -a
source "$ENV_LOCAL"
set +a

LAN_IP="$(hostname -I 2>/dev/null | awk '{print $1}')"
if [[ -z "${LAN_IP}" ]]; then
  LAN_IP="$(ip -4 route get 1.1.1.1 2>/dev/null | awk '{for (i=1;i<=NF;i++) if ($i=="src") {print $(i+1); exit}}')"
fi

echo
echo "======== 本机接入信息（写入 ESP32）========"
echo "  WebSocket : ws://${LAN_IP:-<LAN_IP>}:${DEVICE_PORT}/api/realtime"
echo "  Token     : ${DEVICE_ACCESS_TOKEN}"
echo "  Gateway   : http://${LAN_IP:-<LAN_IP>}:18888"
echo "=========================================="
echo

mkdir -p "$DEPLOY/logs"
GW_LOG="$DEPLOY/logs/gateway.log"
RELAY_LOG="$DEPLOY/logs/relay.log"

cleanup() {
  echo "正在停止..."
  if [[ -n "${GW_PID:-}" ]]; then kill "$GW_PID" 2>/dev/null || true; fi
  if [[ -n "${RELAY_PID:-}" ]]; then kill "$RELAY_PID" 2>/dev/null || true; fi
}
trap cleanup EXIT INT TERM

echo "==> 启动 Gateway → $GW_LOG"
cd "$REPO"
node cli/bin/qwenaudio.mjs gateway run --url http://127.0.0.1:18888 --lan >>"$GW_LOG" 2>&1 &
GW_PID=$!

for _ in $(seq 1 40); do
  if curl -sf http://127.0.0.1:18888/api/health >/dev/null 2>&1; then
    echo "Gateway 已就绪"
    break
  fi
  if ! kill -0 "$GW_PID" 2>/dev/null; then
    echo "Gateway 进程已退出，见 $GW_LOG" >&2
    tail -n 40 "$GW_LOG" >&2 || true
    exit 1
  fi
  sleep 0.5
done

echo "==> 启动 device-relay → $RELAY_LOG"
cd "$REPO"
node --env-file="$ENV_LOCAL" examples/ai-passport/device-relay.mjs >>"$RELAY_LOG" 2>&1 &
RELAY_PID=$!
sleep 0.8
if ! kill -0 "$RELAY_PID" 2>/dev/null; then
  echo "device-relay 启动失败，见 $RELAY_LOG" >&2
  tail -n 40 "$RELAY_LOG" >&2 || true
  exit 1
fi

echo
echo "Gateway PID=$GW_PID  relay PID=$RELAY_PID"
echo "Ctrl-C 结束两个进程。"
echo "本机 WebUI：http://127.0.0.1:18888"
echo "局域网 WebUI：http://${LAN_IP:-<LAN_IP>}:18888"
echo "测固件前请关掉 WebUI 页面。"
wait
