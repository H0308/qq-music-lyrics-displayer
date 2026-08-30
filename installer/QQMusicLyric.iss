; QQMusicLyric Inno Setup installer
; 编译前建议将官方 vc_redist.x64.exe 放到 installer\prereq\。
; 如果该文件不存在，脚本仍可编译，但生成的安装包要求目标系统已有
; Visual C++ x64 Redistributable；正式发布前应补齐该前置依赖。

#define AppVersion "2.1.0"
#define BuildDir "..\build\x64-Release"
#define ProjectDir ExtractFileDir(ExtractFileDir(AddBackslash(SourcePath) + "QQMusicLyric.iss"))
#define BuildDirPath AddBackslash(ProjectDir) + "build\x64-Release"
#define BuildExe BuildDirPath + "\QQMusicLyric.exe"

#ifnexist "prereq\vc_redist.x64.exe"
#warning "Missing installer\prereq\vc_redist.x64.exe; the generated installer will not install the MSVC runtime."
#endif

#if FileExists(BuildExe) == 0
#error "Release executable not found: build\x64-Release\QQMusicLyric.exe"
#endif

; Compile-time guard: --verify-release returns 0 only for a Release build.
#if Exec(BuildExe, "--verify-release", BuildDirPath, 1, 0) != 0
#error "QQMusicLyric.exe is not a Release build; rebuild the x64-Release configuration before packaging."
#endif

[Setup]
AppId=QQMusicLyric
AppName=QQMusicLyric
AppVersion={#AppVersion}
AppPublisher=H0308
AppPublisherURL=https://github.com/H0308/qq-music-lyrics-displayer
DefaultDirName={autopf}\QQMusicLyric
DefaultGroupName=QQMusicLyric
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
OutputDir=..\dist
OutputBaseFilename=QQMusicLyric-{#AppVersion}-Setup
SetupIconFile=..\asset\logo-dark.ico
UninstallDisplayIcon={app}\QQMusicLyric.exe
UninstallDisplayName=QQMusicLyric
; 默认不自动关闭正在运行的程序；软件内更新会通过 /CLOSEAPPLICATIONS 显式覆盖。
CloseApplications=no
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
VersionInfoVersion={#AppVersion}.0
VersionInfoCompany=H0308
VersionInfoDescription=QQMusicLyric 安装程序
VersionInfoProductName=QQMusicLyric

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Files]
Source: "{#BuildDir}\QQMusicLyric.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\libcurl.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\z.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"

#ifexist "prereq\vc_redist.x64.exe"
Source: "prereq\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall
#endif

[Icons]
Name: "{autoprograms}\QQMusicLyric"; Filename: "{app}\QQMusicLyric.exe"; WorkingDir: "{app}"

[Run]
#ifexist "prereq\vc_redist.x64.exe"
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "正在安装 Microsoft Visual C++ 运行库..."; Flags: waituntilterminated
#endif
Filename: "{app}\QQMusicLyric.exe"; Description: "启动 QQMusicLyric"; Flags: nowait postinstall skipifsilent

[Code]
const
  AutoStartKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  AutoStartValue = 'QQMusicLyric';
  AppExeName = 'QQMusicLyric.exe';

{ 通过 WMI 按可执行文件名精确枚举进程；WMI 不可用时返回 False 以免误拦截安装。 }
function IsAppRunning: Boolean;
var
  WbemLocator: Variant;
  WbemServices: Variant;
  Processes: Variant;
begin
  Result := False;
  try
    WbemLocator := CreateOleObject('WbemScripting.SWbemLocator');
    WbemServices := WbemLocator.ConnectServer('.', 'root\CIMV2');
    Processes := WbemServices.ExecQuery(
      'SELECT ProcessId FROM Win32_Process WHERE Name = ''' + AppExeName + '''');
    Result := Processes.Count > 0;
  except
    Result := False;
  end;
end;

{ 软件内更新由 C++ 传入 Inno Setup 官方 /CLOSEAPPLICATIONS 参数。
  外部直接运行安装包不带该参数，继续走手动退出流程。 }
function IsInAppUpdate: Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
  begin
    if CompareText(ParamStr(I), '/CLOSEAPPLICATIONS') = 0 then
    begin
      Result := True;
      exit;
    end;
  end;
end;

{ 外部安装包启动前拦截：进程仍在运行时提示用户先手动退出，可重试或取消安装。 }
function InitializeSetup: Boolean;
begin
  Result := True;
  if IsInAppUpdate then
    exit;

  while IsAppRunning do
  begin
    if MsgBox('检测到 QQMusicLyric 正在运行。' + #13#10 + #13#10 +
      '请先在系统托盘图标上右键退出软件，然后点击“重试”继续安装；' +
      '点击“取消”将退出安装程序。',
      mbError, MB_RETRYCANCEL) = IDCANCEL then
    begin
      Result := False;
      exit;
    end;
  end;
end;

{ 外部安装包点击“安装”后再次确认：覆盖向导停留期间用户重新打开软件的情况。 }
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if IsInAppUpdate then
    exit;

  while IsAppRunning do
  begin
    if MsgBox('检测到 QQMusicLyric 正在运行。' + #13#10 + #13#10 +
      '请先在系统托盘图标上右键退出软件，然后点击“重试”继续安装；' +
      '点击“取消”将退出安装程序。',
      mbError, MB_RETRYCANCEL) = IDCANCEL then
    begin
      Result := '安装已取消：QQMusicLyric 正在运行。';
      exit;
    end;
  end;
end;

procedure MigrateAutoStartPath;
var
  ExistingCommand: String;
  NewCommand: String;
begin
  { 只迁移已经存在的自启动值，不把未启用自启动的用户改成启用。 }
  if not RegQueryStringValue(HKEY_CURRENT_USER, AutoStartKey, AutoStartValue,
    ExistingCommand) then
    exit;

  { 当前程序写入的值就是带引号的 exe 路径；安装后改成新安装目录。 }
  NewCommand := '"' + ExpandConstant('{app}\QQMusicLyric.exe') + '"';
  RegWriteStringValue(HKEY_CURRENT_USER, AutoStartKey, AutoStartValue, NewCommand);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    MigrateAutoStartPath;
end;
