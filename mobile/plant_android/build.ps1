# 植小伴 手机端 App —— 手工打包脚本（不依赖 Gradle）
# aapt2 compile -> aapt2 link -> javac -> d8 -> 塞入 classes.dex -> zipalign -> apksigner
$ErrorActionPreference = 'Stop'

$ROOT = Split-Path -Parent $MyInvocation.MyCommand.Path   # 脚本所在目录，无需改路径
# JDK：优先用 ZXB_BUILDTOOLS 指定目录，否则用 JAVA_HOME
$BUILDTOOLS = if ($env:ZXB_BUILDTOOLS) { $env:ZXB_BUILDTOOLS } else { $env:JAVA_HOME }
$SDK = Join-Path $env:LOCALAPPDATA 'Android\Sdk'
$BT = Join-Path $SDK 'build-tools\34.0.0'
$PLAT = Join-Path $SDK 'platforms\android-35\android.jar'

$jdkDir = if (Test-Path $BUILDTOOLS) { (Get-ChildItem $BUILDTOOLS -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -like 'jdk-17*' } | Select-Object -First 1) } else { $null }
if ($jdkDir) { $jdkHome = $jdkDir.FullName }
elseif ($env:JAVA_HOME) { $jdkHome = $env:JAVA_HOME }
else { throw '找不到 JDK 17，请设置 JAVA_HOME 或 ZXB_BUILDTOOLS' }
$env:JAVA_HOME = $jdkHome
$env:PATH = (Join-Path $jdkHome 'bin') + ';' + $env:PATH
$JAVAC = Join-Path $jdkHome 'bin\javac.exe'
$KEYTOOL = Join-Path $jdkHome 'bin\keytool.exe'
$AAPT2 = Join-Path $BT 'aapt2.exe'
$D8 = Join-Path $BT 'd8.bat'
$ZIPALIGN = Join-Path $BT 'zipalign.exe'
$APKSIGNER = Join-Path $BT 'apksigner.bat'

foreach ($p in @($AAPT2, $D8, $ZIPALIGN, $APKSIGNER, $PLAT)) { if (-not (Test-Path $p)) { throw ('missing: ' + $p) } }

$OUT = Join-Path $ROOT 'build'
$want = Join-Path $ROOT 'build'
if (Test-Path $OUT) { $resolved = (Resolve-Path $OUT).Path; if ($resolved -ne $want) { throw ('refuse to clean: ' + $resolved) }; Remove-Item -LiteralPath $resolved -Recurse -Force }
foreach ($d in @('gen','classes','dex')) { New-Item -ItemType Directory -Force -Path (Join-Path $OUT $d) | Out-Null }

Write-Host '== 1/7 aapt2 compile =='
& $AAPT2 compile --dir (Join-Path $ROOT 'res') -o (Join-Path $OUT 'res.zip')
if ($LASTEXITCODE -ne 0) { throw 'aapt2 compile failed' }

Write-Host '== 2/7 aapt2 link =='
& $AAPT2 link -o (Join-Path $OUT 'base.apk') -I $PLAT --manifest (Join-Path $ROOT 'AndroidManifest.xml') (Join-Path $OUT 'res.zip') --java (Join-Path $OUT 'gen') --min-sdk-version 24 --target-sdk-version 34 --version-code 3 --version-name 1.2
if ($LASTEXITCODE -ne 0) { throw 'aapt2 link failed' }

Write-Host '== 3/7 javac =='
$srcs = @()
$srcs += (Get-ChildItem -Recurse (Join-Path $ROOT 'src') -Filter *.java | ForEach-Object { $_.FullName })
$srcs += (Get-ChildItem -Recurse (Join-Path $OUT 'gen') -Filter *.java | ForEach-Object { $_.FullName })
& $JAVAC -encoding UTF-8 -nowarn -source 8 -target 8 -bootclasspath $PLAT -classpath $PLAT -d (Join-Path $OUT 'classes') $srcs
if ($LASTEXITCODE -ne 0) { throw 'javac failed' }

Write-Host '== 4/7 d8 =='
$classes = (Get-ChildItem -Recurse (Join-Path $OUT 'classes') -Filter *.class | ForEach-Object { $_.FullName })
& $D8 --lib $PLAT --min-api 24 --output (Join-Path $OUT 'dex') $classes
if ($LASTEXITCODE -ne 0) { throw 'd8 failed' }

Write-Host '== 5/7 打包 classes.dex =='
Add-Type -AssemblyName System.IO.Compression.FileSystem
$apkPath = Join-Path $OUT 'base.apk'
$zip = [System.IO.Compression.ZipFile]::Open($apkPath, 'Update')
$entry = $zip.CreateEntry('classes.dex', [System.IO.Compression.CompressionLevel]::Optimal)
$es = $entry.Open()
$dexBytes = [System.IO.File]::ReadAllBytes((Join-Path $OUT 'dex\classes.dex'))
$es.Write($dexBytes, 0, $dexBytes.Length)
$es.Dispose()
$zip.Dispose()

Write-Host '== 6/7 zipalign =='
& $ZIPALIGN -f -p 4 $apkPath (Join-Path $OUT 'aligned.apk')
if ($LASTEXITCODE -ne 0) { throw 'zipalign failed' }

Write-Host '== 7/7 签名 =='
$ks = Join-Path $ROOT 'zxb.keystore'
if (-not (Test-Path $ks)) {
  & $KEYTOOL -genkeypair -v -keystore $ks -alias zxb -keyalg RSA -keysize 2048 -validity 10000 -storepass zxb123456 -keypass zxb123456 -dname 'CN=ZhiXiaoBan, OU=App, O=ZhiXiaoBan, L=Hangzhou, ST=Zhejiang, C=CN'
  if ($LASTEXITCODE -ne 0) { throw 'keytool failed' }
}
& $APKSIGNER sign --ks $ks --ks-key-alias zxb --ks-pass pass:zxb123456 --key-pass pass:zxb123456 --min-sdk-version 23 --v1-signing-enabled true --v2-signing-enabled true --v3-signing-enabled true --out (Join-Path $OUT 'ZhiXiaoBan.apk') (Join-Path $OUT 'aligned.apk')
if ($LASTEXITCODE -ne 0) { throw 'apksigner failed' }
& $APKSIGNER verify --verbose (Join-Path $OUT 'ZhiXiaoBan.apk')

$final = Join-Path $OUT 'ZhiXiaoBan.apk'
Write-Host ''
Write-Host ('APK   : ' + $final)
Write-Host ('SIZE  : ' + (Get-Item $final).Length)
Write-Host ('SHA256: ' + (Get-FileHash -Algorithm SHA256 -LiteralPath $final).Hash.ToLower())
