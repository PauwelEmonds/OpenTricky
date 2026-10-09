@echo off
rem pack_iso.bat -- the Android APK with your SSX Tricky disc image inside, so the phone needs no file picking.
rem
rem   Drag "SSX Tricky (USA).iso" onto this file, or:
rem   pack_iso.bat "C:\path\to\SSX Tricky (USA).iso" [in.apk] [out.apk]
rem
rem in.apk defaults to OpenTricky-android-debug.apk beside this file (build.sh / gradlew assembleDebug),
rem out.apk to OpenTricky-android-SSX-Tricky.apk beside it. Needs the Android SDK (build-tools) and a JDK 11+,
rem as the Android build does; signs with the same debug key, so it installs over the APK built before.
rem The result has your copy of the game in it: it is for your own phone, not for sharing.
setlocal EnableExtensions DisableDelayedExpansion

set "ISO=%~f1"
set "APK=%~f2"
set "OUT=%~f3"
if not defined APK set "APK=%~dp0OpenTricky-android-debug.apk"
if not defined OUT set "OUT=%~dp0OpenTricky-android-SSX-Tricky.apk"
if not defined ISO if exist "%~dp0SSX Tricky (USA).iso" set "ISO=%~dp0SSX Tricky (USA).iso"
if not defined ISO (
    echo Drag your "SSX Tricky (USA).iso" onto pack_iso.bat, or type its path here.
    set /p "ISO=Disc image: "
)
if not defined ISO goto usage
set "ISO=%ISO:"=%"
if not exist "%ISO%" (
    echo Disc image not found: "%ISO%"
    goto fail
)
if not exist "%APK%" (
    echo APK not found: "%APK%"
    echo Build it first: build.sh android, or android\gradlew assembleDebug.
    goto fail
)

rem ---- the tools -----------------------------------------------------------------------------------------------------
set "SDK=%ANDROID_HOME%"
if not defined SDK set "SDK=%ANDROID_SDK_ROOT%"
if not defined SDK set "SDK=%LOCALAPPDATA%\Android\Sdk"
set "BT="
for /f "delims=" %%d in ('dir /b /ad /o:n "%SDK%\build-tools" 2^>nul') do (
    if exist "%SDK%\build-tools\%%d\zipalign.exe" if exist "%SDK%\build-tools\%%d\apksigner.bat" set "BT=%SDK%\build-tools\%%d"
)
if not defined BT (
    echo No Android build-tools found in "%SDK%". Set ANDROID_HOME to your Android SDK.
    goto fail
)

set "JAVA="
if defined JAVA_HOME if exist "%JAVA_HOME%\bin\java.exe" set "JAVA=%JAVA_HOME%\bin\java.exe"
if not defined JAVA if exist "%ProgramFiles%\Android\Android Studio\jbr\bin\java.exe" set "JAVA=%ProgramFiles%\Android\Android Studio\jbr\bin\java.exe"
if not defined JAVA for /f "delims=" %%j in ('where java 2^>nul') do if not defined JAVA set "JAVA=%%j"
if not defined JAVA (
    echo No Java found. Install Android Studio, or set JAVA_HOME to a JDK 11 or newer.
    goto fail
)
for %%j in ("%JAVA%") do for %%h in ("%%~dpj..") do set "JAVA_HOME=%%~fh"

set "KS=%USERPROFILE%\.android\debug.keystore"
if not exist "%KS%" (
    echo Making the debug signing key, %KS%
    if not exist "%USERPROFILE%\.android" mkdir "%USERPROFILE%\.android"
    "%JAVA_HOME%\bin\keytool.exe" -genkeypair -keystore "%KS%" -storepass android -alias androiddebugkey ^
        -keypass android -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" || goto fail
)

echo APK:        %APK%
echo Disc image: %ISO%
echo Output:     %OUT%
echo.

rem ---- pack, align, sign ---------------------------------------------------------------------------------------------
set "T1=%OUT%.packed.tmp"
set "T2=%OUT%.aligned.tmp"
"%JAVA%" "%~dp0android\tools\PackIso.java" "%APK%" "%ISO%" "%T1%" || goto fail_tmp
echo Aligning...
"%BT%\zipalign.exe" -f -p 4 "%T1%" "%T2%" || goto fail_tmp
del "%T1%"
echo Signing...
call "%BT%\apksigner.bat" sign --ks "%KS%" --ks-pass pass:android --ks-key-alias androiddebugkey --key-pass pass:android ^
    --v4-signing-enabled false --out "%OUT%" "%T2%" || goto fail_tmp
del "%T2%"

echo.
echo Done: %OUT%
echo Install it with the phone connected (USB debugging on):
echo     adb install -r "%OUT%"
echo or copy it to the phone and open it there. It has your game in it: keep it to yourself.
goto end

:usage
echo Usage: pack_iso.bat "SSX Tricky (USA).iso" [in.apk] [out.apk]
goto fail

:fail_tmp
if exist "%T1%" del "%T1%"
if exist "%T2%" del "%T2%"
:fail
echo.
echo Packing failed.
pause
exit /b 1

:end
pause
exit /b 0
