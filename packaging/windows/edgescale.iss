; EdgeScale Windows installer (Inno Setup 6+)
; Build after scripts\build-windows.bat has produced dist\edgescale-windows-x86_64\

#ifndef MyAppVersion
  #define MyAppVersion "0.8.3"
#endif

#ifndef SourceDir
  #define SourceDir "..\..\dist\edgescale-windows-x86_64"
#endif

#ifndef OutputDir
  #define OutputDir "..\..\dist"
#endif

#define MyAppName "EdgeScale"
#define MyAppPublisher "EdgeScale"
#define MyAppURL "https://github.com/jersobh/edge"
#define MyAppExeName "install-windows.bat"

[Setup]
AppId={{A8E3C1F2-9B4D-4E7A-8C21-EDGESCALE0001}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}/releases
DefaultDirName={localappdata}\EdgeScale
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
; Per-user install — matches bat installer (HKCU ImplicitLayers, %LOCALAPPDATA%)
PrivilegesRequired=lowest
OutputDir={#OutputDir}
OutputBaseFilename=EdgeScale-{#MyAppVersion}-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#MyAppName} {#MyAppVersion}
UninstallDisplayIcon={app}\edgescale.ico
InfoAfterFile=
SetupLogging=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "envenable"; Description: "Set user environment variable ENABLE_EDGESCALE=1 (enable for all Vulkan apps)"; GroupDescription: "Optional:"; Flags: unchecked

[Files]
Source: "{#SourceDir}\VkLayer_XFSR.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\VkLayer_EdgeScale.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\VkLayer_XFSR.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\shaders\*"; DestDir: "{app}\shaders"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\install-windows.bat"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\uninstall-windows.bat"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\README.md"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{group}\EdgeScale README"; Filename: "{app}\README.md"; Flags: preventpinning
Name: "{group}\Uninstall EdgeScale"; Filename: "{uninstallexe}"

[Registry]
; Khronos implicit Vulkan layer registration (same as install-windows.bat)
Root: HKCU; Subkey: "Software\Khronos\Vulkan\ImplicitLayers"; ValueType: dword; ValueName: "{app}\VkLayer_EdgeScale.json"; ValueData: 0; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Khronos\Vulkan\ImplicitLayers"; ValueType: dword; ValueName: "{app}\VkLayer_XFSR.json"; ValueData: 0; Flags: uninsdeletevalue
; Optional global enable
Root: HKCU; Subkey: "Environment"; ValueType: string; ValueName: "ENABLE_EDGESCALE"; ValueData: "1"; Tasks: envenable; Flags: uninsdeletevalue

[Code]
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
  begin
    { Best-effort cleanup if bats left stray keys }
    RegDeleteValue(HKEY_CURRENT_USER, 'Software\Khronos\Vulkan\ImplicitLayers',
      ExpandConstant('{app}\VkLayer_EdgeScale.json'));
    RegDeleteValue(HKEY_CURRENT_USER, 'Software\Khronos\Vulkan\ImplicitLayers',
      ExpandConstant('{app}\VkLayer_XFSR.json'));
  end;
end;

[Messages]
FinishedLabel=EdgeScale is installed.%n%nEnable per game with ENABLE_EDGESCALE=1 in Steam launch options:%n  cmd /c "set ENABLE_EDGESCALE=1 && %%command%%"%n%nHotkeys: F1 preset, F2 RCAS/edge mode, F3 upscaler, F10 overlay.
