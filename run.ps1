Set-Location $PSScriptRoot

$midiFile = "$PSScriptRoot\assets\midi\canon_in_d.mid"

# build
C:\raylib\w64devkit\bin\make.exe

# run both in parallel
$synesthesia = Start-Process -FilePath ".\synesthesia.exe" -PassThru
$midi        = Start-Process -FilePath "C:\Program Files\Windows Media Player\wmplayer.exe" `
                             -ArgumentList "`"$midiFile`"" `
                             -PassThru

$synesthesia.WaitForExit()
$midi.WaitForExit()
