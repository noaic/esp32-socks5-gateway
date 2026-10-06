param(
    [string]$ClientIP = "",
    [string]$RemoteIP = "198.51.100.7",
    [int]$Port = 443,
    [int]$Rounds = 64
)
$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($ClientIP)) {
    $addresses = @(Get-NetIPAddress -AddressFamily IPv4 | Where-Object {
        $_.AddressState -eq "Preferred" -and $_.IPAddress -match '^192\.168\.4\.(\d+)$' -and
        [int]$Matches[1] -ge 2 -and [int]$Matches[1] -le 254
    })
    if ($addresses.Count -ne 1) {
        Get-NetIPAddress -AddressFamily IPv4 | Select-Object InterfaceAlias, IPAddress | Out-Host
        throw "Expected one Windows AP address in 192.168.4.2..254. Connect this Windows PC to the ESP32 AP, then retry."
    }
    $ClientIP = $addresses[0].IPAddress
    Write-Host "Using AP interface $($addresses[0].InterfaceAlias), IPv4=$ClientIP"
}
$client = New-Object System.Net.Sockets.TcpClient([System.Net.Sockets.AddressFamily]::InterNetwork)
try {
    $client.Client.Bind((New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Parse($ClientIP), 0)))
    $async = $client.BeginConnect($RemoteIP, $Port, $null, $null)
    try {
        if (-not $async.AsyncWaitHandle.WaitOne(8000)) { throw "Connect timeout: check AP connection and route" }
        $client.EndConnect($async)
    } finally { $async.AsyncWaitHandle.Close() }
    $stream = $client.GetStream()
    $stream.ReadTimeout = 8000
    $stream.WriteTimeout = 8000
    Write-Host "Local=$($client.Client.LocalEndPoint) Remote=$($client.Client.RemoteEndPoint)"
    if ($client.Client.RemoteEndPoint.Address.ToString() -ne $RemoteIP -or
        $client.Client.RemoteEndPoint.Port -ne $Port) { throw "Remote endpoint changed" }
    $rng = New-Object System.Random(20261002)
    $total = 0
    foreach ($length in (@(1, 37, 4096, 16384) + (@(1024) * $Rounds))) {
        $data = New-Object byte[] $length
        $rng.NextBytes($data)
        $stream.Write($data, 0, $data.Length)
        $reply = New-Object byte[] $length
        $offset = 0
        while ($offset -lt $reply.Length) {
            $n = $stream.Read($reply, $offset, $reply.Length - $offset)
            if ($n -eq 0) { throw "Unexpected EOF" }
            $offset += $n
        }
        for ($i = 0; $i -lt $length; $i++) {
            if ($data[$i] -ne $reply[$i]) { throw "Payload mismatch at byte $i" }
        }
        $total += $length
    }
    $client.Client.Shutdown([System.Net.Sockets.SocketShutdown]::Send)
    $last = New-Object byte[] 1
    if ($stream.Read($last, 0, 1) -ne 0) { throw "Expected server FIN/EOF" }
    Write-Host "PASS: $total bytes echoed exactly; original remote endpoint preserved; FIN/EOF received"
} finally { $client.Close() }
