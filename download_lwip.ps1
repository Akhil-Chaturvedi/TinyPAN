# TinyPAN lwIP Download Script
# Downloads only the essential lwIP files needed for TinyPAN
# Total download: ~200KB instead of full 2MB+ repo

param(
    [string]$LwipDir = "lib\lwip"
)

$BaseUrl = "https://raw.githubusercontent.com/lwip-tcpip/lwip/master"

# Essential files list
$Files = @(
    # Core
    "src/core/init.c",
    "src/core/def.c",
    "src/core/mem.c",
    "src/core/memp.c",
    "src/core/netif.c",
    "src/core/pbuf.c",
    "src/core/timeouts.c",
    
    # IPv4
    "src/core/ipv4/etharp.c",
    "src/core/ipv4/ip4.c",
    "src/core/ipv4/ip4_addr.c",
    "src/core/ipv4/ip4_frag.c",
    "src/core/ipv4/icmp.c",
    "src/core/ipv4/dhcp.c",
    
    # UDP/TCP
    "src/core/udp.c",
    "src/core/tcp.c",
    "src/core/tcp_in.c",
    "src/core/tcp_out.c",
    "src/core/dns.c",
    "src/core/inet_chksum.c",
    
    # Headers - include/lwip
    "src/include/lwip/opt.h",
    "src/include/lwip/def.h",
    "src/include/lwip/mem.h",
    "src/include/lwip/memp.h",
    "src/include/lwip/netif.h",
    "src/include/lwip/pbuf.h",
    "src/include/lwip/timeouts.h",
    "src/include/lwip/init.h",
    "src/include/lwip/err.h",
    "src/include/lwip/debug.h",
    "src/include/lwip/stats.h",
    "src/include/lwip/sys.h",
    "src/include/lwip/ip.h",
    "src/include/lwip/ip4.h",
    "src/include/lwip/ip4_addr.h",
    "src/include/lwip/ip_addr.h",
    "src/include/lwip/ip4_frag.h",
    "src/include/lwip/etharp.h",
    "src/include/lwip/icmp.h",
    "src/include/lwip/dhcp.h",
    "src/include/lwip/udp.h",
    "src/include/lwip/tcp.h",
    "src/include/lwip/dns.h",
    "src/include/lwip/inet_chksum.h",
    "src/include/lwip/prot/ethernet.h",
    "src/include/lwip/prot/etharp.h",
    "src/include/lwip/prot/ip4.h",
    "src/include/lwip/prot/icmp.h",
    "src/include/lwip/prot/dhcp.h",
    "src/include/lwip/prot/udp.h",
    "src/include/lwip/prot/tcp.h",
    "src/include/lwip/prot/dns.h",
    "src/include/lwip/prot/iana.h",
    "src/include/lwip/prot/ieee.h",
    "src/include/lwip/arch.h",
    "src/include/lwip/priv/memp_std.h",
    "src/include/lwip/priv/memp_priv.h",
    "src/include/lwip/priv/mem_priv.h",
    "src/include/lwip/priv/tcp_priv.h",
    
    # netif headers
    "src/include/netif/ethernet.h",
    
    # netif source
    "src/netif/ethernet.c"
)

Write-Host "TinyPAN lwIP Downloader" -ForegroundColor Cyan
Write-Host "=======================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Downloading essential lwIP files (~200KB total)..."
Write-Host ""

$downloaded = 0
$failed = 0

foreach ($file in $Files) {
    $url = "$BaseUrl/$file"
    $localPath = Join-Path $LwipDir $file
    $localDir = Split-Path $localPath -Parent
    
    # Create directory if needed
    if (-not (Test-Path $localDir)) {
        New-Item -ItemType Directory -Path $localDir -Force | Out-Null
    }
    
    Write-Host "  $file... " -NoNewline
    
    try {
        Invoke-WebRequest -Uri $url -OutFile $localPath -UseBasicParsing -ErrorAction Stop
        Write-Host "OK" -ForegroundColor Green
        $downloaded++
    }
    catch {
        Write-Host "FAILED" -ForegroundColor Red
        $failed++
    }
}

Write-Host ""
Write-Host "Download complete: $downloaded files downloaded, $failed failed" -ForegroundColor $(if ($failed -eq 0) { "Green" } else { "Yellow" })
Write-Host ""
