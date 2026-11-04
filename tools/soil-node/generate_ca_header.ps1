param(
  [Parameter(Mandatory=$true)][string]$PemPath,
  [Parameter(Mandatory=$true)][string]$OutPath
)

$content = [IO.File]::ReadAllText((Resolve-Path $PemPath)).Replace("`r`n", "`n").TrimEnd()
$lines = $content -split "`n"
$literals = $lines | ForEach-Object {
  '  "' + $_.Replace('\', '\\').Replace('"', '\"') + '\n"'
}

$result = @(
  '#pragma once',
  '',
  '/* Public Let''s Encrypt ISRG Root X1 trust anchor. */',
  'static const char SOIL_MQTT_ROOT_CA_PEM[] =',
  ($literals -join "`n"),
  '  ;',
  ''
)

[IO.File]::WriteAllText($OutPath, ($result -join "`n"), [Text.UTF8Encoding]::new($false))
