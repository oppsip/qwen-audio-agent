# Run in Windows PowerShell as Administrator so the ESP32 on Wi-Fi/Ethernet
# can reach the Gateway running inside WSL2.
#
# Usage:
#   powershell.exe -ExecutionPolicy Bypass -File deploy/wsl-portproxy.ps1
#
# Then point the ESP32 qwen_host at the *Windows* LAN IPv4 (192.168.x.x),
# not the WSL address (172.x.x.x).

$wsl = (wsl hostname -I).Trim().Split(' ')[0]
if (-not $wsl) { throw 'Could not read WSL IPv4' }

foreach ($port in 3101, 18888) {
  netsh interface portproxy delete v4tov4 listenport=$port listenaddress=0.0.0.0 | Out-Null
  netsh interface portproxy add v4tov4 listenport=$port listenaddress=0.0.0.0 connectport=$port connectaddress=$wsl
  Write-Host "portproxy 0.0.0.0:$port -> ${wsl}:$port"
}

New-NetFirewallRule -DisplayName 'Qwen Audio Agent LAN' -Direction Inbound -Protocol TCP -LocalPort 3101,18888 -Action Allow -ErrorAction SilentlyContinue | Out-Null
Write-Host 'Firewall rule ensured for TCP 3101 and 18888'
netsh interface portproxy show all
