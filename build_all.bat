@echo off
rem Imtidad 2.0.1: builds the Android app, then the Windows installer, on this PC.
rem Uses the JDK and Android SDK in Documents\ScreenLink-tools\toolchain, and NSIS (makensis).
setlocal enabledelayedexpansion
chcp 65001 >nul
set "ROOT=%~dp0"
set "TOOLS=%USERPROFILE%\Documents\ScreenLink-tools\toolchain"
set "LOG=%ROOT%build_all_log.txt"
echo Imtidad build %date% %time% > "%LOG%"

for /d %%D in ("%TOOLS%\*") do (
  if exist "%%D\bin\java.exe" set "JAVA_HOME=%%D"
  if exist "%%D\platforms" set "ANDROID_HOME=%%D"
  if exist "%%D\platform-tools" set "ANDROID_HOME=%%D"
)
if not defined JAVA_HOME ( echo JDK not found in %TOOLS% >> "%LOG%" & echo لم يُعثر على JDK في %TOOLS% & goto :end )
if not defined ANDROID_HOME ( echo Android SDK not found in %TOOLS% >> "%LOG%" & echo لم يُعثر على Android SDK في %TOOLS% & goto :end )
set "ANDROID_SDK_ROOT=%ANDROID_HOME%"
echo JAVA_HOME=%JAVA_HOME% >> "%LOG%"
echo ANDROID_HOME=%ANDROID_HOME% >> "%LOG%"

echo [1/2] بناء تطبيق أندرويد...
pushd "%ROOT%android"
call gradlew.bat assembleRelease --no-daemon >> "%LOG%" 2>&1
set "RC=%errorlevel%"
popd
if not "%RC%"=="0" ( echo فشل بناء التطبيق. انظر build_all_log.txt & goto :end )
set "APK="
for %%F in ("%ROOT%android\app\build\outputs\apk\release\*.apk") do set "APK=%%F"
if not defined APK ( echo APK not produced >> "%LOG%" & echo لم يُنتج ملف APK & goto :end )
copy /y "%APK%" "%ROOT%installer\payload\Imtidad-2.0.1.apk" >nul
copy /y "%APK%" "%ROOT%Imtidad-2.0.1.apk" >nul
echo APK: %ROOT%Imtidad-2.0.1.apk >> "%LOG%"

echo [2/2] بناء المثبّت...
set "NSIS="
for %%P in ("%TOOLS%\nsis\makensis.exe" "%ProgramFiles(x86)%\NSIS\makensis.exe" "%ProgramFiles%\NSIS\makensis.exe") do if exist %%P set "NSIS=%%~P"
if not defined NSIS (
  echo NSIS not found >> "%LOG%"
  echo التطبيق جاهز: Imtidad-2.0.1.apk
  echo لبناء المثبّت ثبّت NSIS من nsis.sourceforge.io أو بالأمر: winget install NSIS.NSIS ثم شغّل هذا الملف مرة أخرى.
  goto :end
)
pushd "%ROOT%installer"
"%NSIS%" imtidad.nsi >> "%LOG%" 2>&1
set "RC=%errorlevel%"
popd
if not "%RC%"=="0" ( echo فشل بناء المثبّت. انظر build_all_log.txt & goto :end )
move /y "%ROOT%installer\Imtidad-Setup-2.0.1.exe" "%ROOT%Imtidad-Setup-2.0.1.exe" >nul
echo Setup: %ROOT%Imtidad-Setup-2.0.1.exe >> "%LOG%"
echo تم: Imtidad-Setup-2.0.1.exe و Imtidad-2.0.1.apk في مجلد المشروع.

:end
echo.
pause
