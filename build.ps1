$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $MyInvocation.MyCommand.Path
$zig = if ($env:ZIG) { $env:ZIG } else { 'zig' }
$out = Join-Path $repo 'build\altscreen_handler.so'
New-Item -ItemType Directory -Force -Path (Split-Path $out) | Out-Null
& $zig cc -target aarch64-linux-gnu.2.17 -shared -fPIC -O2 -Wall -Wextra `
  -fno-stack-protector '-Wl,-z,relro,-z,now' -o $out `
  (Join-Path $repo 'src\altscreen_handler.c') -ldl -lpthread
if ($LASTEXITCODE -ne 0) { throw 'Zig build failed' }
& python (Join-Path $repo 'tools\verify_build.py') $out
if ($LASTEXITCODE -ne 0) { throw 'Build verification failed' }
