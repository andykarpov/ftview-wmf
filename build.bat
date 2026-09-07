set SDCC=E:\SDCC\4.0.0
set path=%SDCC%\bin;E:\zx-evo-master\pentevo\tools\hex2bin;%path%

if not exist obj md obj
del /q /s obj >nul

sdasz80 -o obj/crt0.rel src/crt0.s
if errorlevel 1 pause & exit

sdcc -Isrc -I../lib/sdk -I../lib/ft812 -I../lib/tsconf -mz80 --std-sdcc11 --opt-code-speed --no-std-crt0 obj/crt0.rel --code-loc 0x8003 --data-loc 0xA800 src/main.c ../lib/ft812/ft812.lib ../lib/tsconf/ts.lib ../lib/sdk/sdk.lib -o obj/out.hex
if errorlevel 1 pause & exit
if exist obj/out.hex hex2bin obj/out.hex >nul

for %%i in (obj/out.bin) do set sz=%%~zi
set /a sz=(%sz% - 1) / 512 + 1

REM A WC plugin page is exactly 16KB = 32 512-byte blocks (WALK in WCVW.ASM
REM loads N_BLK0 blocks straight off disk into the #C000-#FFFF window as
REM one contiguous chunk - anything past 32 blocks overflows that window).
REM --data-loc 0xAE00 (the value ftview shipped with) left so much unused
REM gap between code and data that the total came to 33 blocks - one over
REM the limit. That silently broke WC's boot-time [PLUGINS] autoload
REM (wc.ini parsing never reached [LPANEL]/[RPANEL]) while F3/open-by-
REM extension still worked, since by then the plugin was already resident
REM and didn't need a fresh disk load. Confirmed on real hardware
REM 2026-08-28 by comparing against the original shipped ftview.wmf (which
REM never got anywhere near 32 blocks) and a from-GitHub unmodified
REM rebuild (which hit the exact same 33-block overflow). 0xA800 leaves
REM enough headroom above current code size without wasting most of a page.
if %sz% GTR 32 (
  echo ERROR: build needs %sz% x 512-byte blocks, but a single WC plugin page holds only 32 - shrink the code or lower --data-loc's headroom above it.
  pause
  exit
)

REM Pad obj/out.bin up to a whole number of 512-byte blocks (sz*512 bytes).
REM hex2bin only emits the bytes actually present in the hex file, which
REM generally is NOT block-aligned, while N_BLK0 below always rounds up -
REM so the unpadded file was technically short of what the header promised
REM (not the cause of the autoload bug above - the original shipped binary
REM has the same kind of shortfall and works fine - but still worth being
REM exact about).
set /a padded=%sz% * 512
powershell -NoProfile -Command "$fs=[System.IO.File]::Open('obj\out.bin',[System.IO.FileMode]::Open,[System.IO.FileAccess]::Write); $fs.SetLength(%padded%); $fs.Close()"

sdcc -DN_BLK0=%sz% -Isrc -I../lib/sdk -I../lib/ft812 -I../lib/tsconf --no-std-crt0 -mz80 --std-sdcc11 --opt-code-speed src/wc_h.c -o obj/hdr.hex
if errorlevel 1 pause & exit
if exist obj/hdr.hex hex2bin obj/hdr.hex >nul

copy /b obj\hdr.bin + obj\out.bin ftview.wmf >nul

REM imgcpy.exe ftview.wmf unreal\wc.img=c:\wc\ftview.wmf
REM if errorlevel 1 pause & exit

REM unreal\unreal
