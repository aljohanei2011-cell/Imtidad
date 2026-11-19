; Imtidad (امتداد) installer — build with: makensis imtidad.nsi
Unicode true
ManifestDPIAware true
SetCompressor /SOLID lzma
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"

!define APPNAME "امتداد"
!define VERSION "2.0.1"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Imtidad"

Name "${APPNAME}"
Caption "تثبيت ${APPNAME} ${VERSION}"
OutFile "Imtidad-Setup-${VERSION}.exe"
InstallDir "$PROGRAMFILES64\Imtidad"
RequestExecutionLevel admin
BrandingText "امتداد ${VERSION} — تطوير عاصم الجهني"

VIProductVersion "2.0.1.0"
VIAddVersionKey /LANG=1025 "ProductName" "Imtidad"
VIAddVersionKey /LANG=1025 "FileDescription" "Imtidad setup"
VIAddVersionKey /LANG=1025 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=1025 "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=1025 "LegalCopyright" "© 2026 عاصم الجهني. جميع الحقوق محفوظة."
VIAddVersionKey /LANG=1025 "CompanyName" "عاصم الجهني"

!define MUI_ICON "screenlink.ico"
!define MUI_UNICON "screenlink.ico"
!define MUI_ABORTWARNING
!define MUI_WELCOMEFINISHPAGE_BITMAP "welcome.bmp"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "welcome.bmp"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_BITMAP "header.bmp"
!define MUI_HEADERIMAGE_RIGHT
!define MUI_BGCOLOR "FFFFFF"
!define MUI_TEXTCOLOR "0F1729"
!define MUI_FINISHPAGE_TITLE "تم التثبيت بنجاح"
!define MUI_WELCOMEPAGE_TITLE "مرحبًا بك في تثبيت امتداد"
!define MUI_WELCOMEPAGE_TEXT "امتداد يجعل التابلت أو الجوال شاشة ثانية لكمبيوترك أو مرآة له، مع التحكم باللمس والقلم ولوحة المفاتيح ونقل الصوت، عبر كيبل USB أو Wi-Fi.$\r$\n$\r$\nسيُثبَّت البرنامج وتعريف الشاشة الثانية، ويُجهَّز تطبيق الأندرويد لتثبيته على أجهزتك.$\r$\n$\r$\nتطوير: عاصم الجهني — © 2026 جميع الحقوق محفوظة."
!define MUI_COMPONENTSPAGE_SMALLDESC
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "تشغيل امتداد الآن"
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchApp
!define MUI_FINISHPAGE_TEXT "اكتمل التثبيت. البرنامج يعمل بجانب الساعة.$\r$\n$\r$\nلتثبيت التطبيق على التابلت أو الجوال: فعّل «تصحيح USB» في خيارات المطوّر على الجهاز، وصّله بالكيبل، ثم من قائمة البرنامج اختر «تثبيت/تحديث التطبيق على التابلت». أو انسخ الملف Imtidad-${VERSION}.apk من مجلد البرنامج إلى الجهاز وافتحه."

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "Arabic"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP|MB_RTLREADING "امتداد يحتاج Windows 10 أو 11 بنظام 64 بت."
    Abort
  ${EndIf}
  SetRegView 64
FunctionEnd

Function LaunchApp
  ; Start without administrator rights (as the signed-in user).
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\Imtidad.exe"'
FunctionEnd

Section "البرنامج (مطلوب)" SecMain
  SectionIn RO
  nsExec::Exec 'taskkill /F /IM Imtidad.exe'
  nsExec::Exec 'taskkill /F /IM screenlink-host.exe'
  Sleep 800
  SetOutPath "$INSTDIR"
  File "payload\Imtidad.exe"
  Delete "$INSTDIR\Imtidad-*.apk"
  File "payload\Imtidad-${VERSION}.apk"
  File "payload\vdd_settings.xml"
  File "payload\LICENSES.txt"
  File "payload\LICENSE.txt"
  SetOutPath "$INSTDIR\adb"
  File "payload\adb\*.*"

  ; Remove the earlier per-user test copy and its shortcuts.
  SetShellVarContext current
  Delete "$SMPROGRAMS\امتداد.lnk"
  Delete "$DESKTOP\امتداد.lnk"
  RMDir /r "$LOCALAPPDATA\Programs\Imtidad"
  WriteRegDWORD HKCU "Software\ScreenLink" "Initialized" 1

  SetShellVarContext all
  CreateShortcut "$SMPROGRAMS\امتداد.lnk" "$INSTDIR\Imtidad.exe" "" "$INSTDIR\Imtidad.exe" 0
  CreateShortcut "$DESKTOP\امتداد.lnk" "$INSTDIR\Imtidad.exe" "" "$INSTDIR\Imtidad.exe" 0

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayName" "امتداد (Imtidad)"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINSTKEY}" "Publisher" "عاصم الجهني"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\Imtidad.exe"
  WriteRegStr HKLM "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTKEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoRepair" 1
  WriteRegDWORD HKLM "${UNINSTKEY}" "EstimatedSize" 12000
SectionEnd

Section "الشاشة الثانية (تعريف الشاشة الافتراضية)" SecDriver
  SetOutPath "$INSTDIR\driver"
  File "payload\driver\*.*"
  DetailPrint "تثبيت تعريف الشاشة الافتراضية…"
  nsExec::ExecToLog 'powershell -NoProfile -ExecutionPolicy Bypass -File "$INSTDIR\driver\install-driver.ps1"'
  Pop $0
  DetailPrint "نتيجة تثبيت التعريف: $0"
SectionEnd

Section "السماح بالاتصال عبر Wi-Fi (الشبكات الخاصة فقط)" SecFirewall
  nsExec::Exec 'netsh advfirewall firewall delete rule name=ScreenLink'
  nsExec::Exec 'netsh advfirewall firewall delete rule name=Imtidad'
  nsExec::Exec 'netsh advfirewall firewall add rule name=Imtidad dir=in action=allow profile=private enable=yes program="$INSTDIR\Imtidad.exe"'
SectionEnd

Section "التشغيل مع Windows" SecAutostart
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "ScreenLink" '"$INSTDIR\Imtidad.exe" --tray'
SectionEnd

LangString DESC_MAIN 1025 "برنامج امتداد مع أداة adb وتطبيق الأندرويد."
LangString DESC_DRIVER 1025 "يضيف شاشة افتراضية لوضع «توسيع الشاشة». مطلوب لاستخدام التابلت كشاشة ثانية."
LangString DESC_FIREWALL 1025 "يسمح للتابلت أو الجوال بالاتصال عبر شبكة Wi-Fi المنزلية."
LangString DESC_AUTOSTART 1025 "يبدأ البرنامج بجانب الساعة عند تشغيل Windows."
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} $(DESC_MAIN)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDriver} $(DESC_DRIVER)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecFirewall} $(DESC_FIREWALL)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecAutostart} $(DESC_AUTOSTART)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Function un.onInit
  SetRegView 64
FunctionEnd

Section "Uninstall"
  nsExec::Exec 'taskkill /F /IM Imtidad.exe'
  Sleep 800
  nsExec::Exec 'netsh advfirewall firewall delete rule name=Imtidad'
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "ScreenLink"
  SetShellVarContext all
  Delete "$SMPROGRAMS\امتداد.lnk"
  Delete "$DESKTOP\امتداد.lnk"
  MessageBox MB_YESNO|MB_DEFBUTTON2|MB_RTLREADING "هل تريد إزالة تعريف الشاشة الافتراضية أيضًا؟ (قد تومض الشاشة لحظة)" IDNO skip_driver
    nsExec::Exec '"$INSTDIR\driver\devcon.exe" remove Root\MttVDD'
  skip_driver:
  RMDir /r "$INSTDIR"
  DeleteRegKey HKLM "${UNINSTKEY}"
SectionEnd
