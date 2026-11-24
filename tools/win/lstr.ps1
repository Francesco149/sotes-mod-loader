# C:\sml-win\lstr.ps1 PORT CODE — like lua.ps1 but the chunk is an argument
param([int]$Port, [string]$Code)
$hex = ([Text.Encoding]::UTF8.GetBytes($Code) | % { $_.ToString('x2') }) -join ''
$c = New-Object Net.Sockets.TcpClient('127.0.0.1', $Port); $c.ReceiveTimeout = 30000
$s = $c.GetStream(); $w = New-Object IO.StreamWriter($s); $w.NewLine = "`n"
$w.WriteLine("lua $hex"); $w.Flush()
$r = New-Object IO.StreamReader($s); $r.ReadLine(); $c.Close()
