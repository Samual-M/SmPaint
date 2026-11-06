; NSIS installer (per-user, no admin rights needed).
; makensis -DARCH=x64 -DVERSION=1.09 -DEXE=build/mspaint-x64.exe -DOUTFILE=dist/mspaintV1.09-win-x64-setup.exe packaging/windows/installer.nsi
Unicode true
!define APPNAME "SmPaint"
Name "${APPNAME} V${VERSION} (${ARCH})"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\SmPaint"
InstallDirRegKey HKCU "Software\Paint" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma
BrandingText "SmPaint V${VERSION}"
Icon "mspaint.ico"
UninstallIcon "mspaint.ico"

Page directory
Page instfiles
UninstPage uninstConfirm
UninstPage instfiles

Section "Install"
  SetOutPath "$INSTDIR"
  File /oname=mspaint.exe "${EXE}"
  File "mspaint.ico"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateShortCut "$SMPROGRAMS\SmPaint.lnk" "$INSTDIR\mspaint.exe" "" "$INSTDIR\mspaint.ico"
  WriteRegStr HKCU "Software\Paint" "InstallDir" "$INSTDIR"
  ; show up in "Open with" for images
  WriteRegStr HKCU "Software\Classes\Applications\mspaint.exe\shell\open\command" "" '"$INSTDIR\mspaint.exe" "%1"'
  WriteRegStr HKCU "Software\Classes\Applications\mspaint.exe" "FriendlyAppName" "SmPaint"
  WriteRegStr HKCU "Software\Classes\Applications\mspaint.exe\SupportedTypes" ".png" ""
  WriteRegStr HKCU "Software\Classes\Applications\mspaint.exe\SupportedTypes" ".bmp" ""
  WriteRegStr HKCU "Software\Classes\Applications\mspaint.exe\SupportedTypes" ".gif" ""
  WriteRegStr HKCU "Software\Classes\Applications\mspaint.exe\SupportedTypes" ".jpg" ""
  WriteRegStr HKCU "Software\Classes\Applications\mspaint.exe\SupportedTypes" ".jpeg" ""
  ; Settings > Apps entry
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Paint" "DisplayName" "SmPaint"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Paint" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Paint" "DisplayIcon" "$INSTDIR\mspaint.ico"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Paint" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Paint" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Paint" "NoRepair" 1
SectionEnd

Section "Uninstall"
  Delete "$INSTDIR\mspaint.exe"
  Delete "$INSTDIR\mspaint.ico"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\SmPaint.lnk"
  DeleteRegKey HKCU "Software\Paint"
  DeleteRegKey HKCU "Software\Classes\Applications\mspaint.exe"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Paint"
SectionEnd
