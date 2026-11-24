# C:\sml-win\lua.ps1 PORT FILE — send a Lua chunk to the loader's dev server (line protocol "lua <hex>"), print the reply.
param([int]$Port, [string]$File)
$code = [IO.File]::ReadAllBytes($File)
$hex = ($code | % { $_.ToString('x2') }) -join ''
$c = New-Object Net.Sockets.TcpClient('127.0.0.1', $Port)
$c.ReceiveTimeout = 30000
$s = $c.GetStream(); $w = New-Object IO.StreamWriter($s); $w.NewLine = "`n"
$w.WriteLine("lua $hex"); $w.Flush()
$r = New-Object IO.StreamReader($s); $r.ReadLine(); $c.Close()
