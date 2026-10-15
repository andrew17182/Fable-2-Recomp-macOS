@echo off
rem Print the SHA-256 of the staged release binaries.
rem Usage:  tools\repro\hashes.cmd [build-dir]
rem   Default build dir: out\build\win-amd64-release
rem Run before and after a rebuild (of identical source) and diff the output:
rem identical source should give identical hashes.
setlocal
cd /d "%~dp0\..\.."
set "BD=%~1"
if "%BD%"=="" set "BD=out\build\win-amd64-release"
if not exist "%BD%" (
  echo Build dir not found: %BD% 1>&2
  exit /b 1
)
echo # %CD%
echo # %BD%
echo #
powershell -NoProfile -Command ^
  "$files = Get-ChildItem -Path '%BD%' -File | Where-Object { $_.Name -match '^(fable_2\.exe|rexruntime.*\.dll|rexgpu-xenos.*\.dll)$' } | Sort-Object Name; { foreach ($f in $files) { (Get-FileHash -Algorithm SHA256 -Path $f.FullName).Hash + '  ' + $f.Name } }"
endlocal
