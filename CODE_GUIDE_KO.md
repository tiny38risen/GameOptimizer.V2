# GameOptimizer.V2 코드 구조 안내

이 문서는 현재 구조를 한국어로 설명한다. 코드 블록은 역할과 데이터 흐름을 보여 주는 축약 의사 코드이며, 그대로 컴파일하는 코드는 아니다. 실제 동작은 `.cs`, `.xaml`, `.cpp`, `.h`, `.vcxproj` 파일을 기준으로 한다.

## 1. 전체 구조

```text
WPF 프론트엔드 (C#)                         네이티브 엔진 (C++)
─────────────────────                       ──────────────────
App.xaml → MainWindow                       go_start_session(...)
              │                                     │
              ├─ GameTarget 목록 표시                └─ GO_SESSION_IMPL 시작
              │                                            │
              ├─ NativeEngineSession ── C ABI/PInvoke ────┤
              │                                            ├─ 스레드 CPU 시간 관찰
              └─ 상태·후보 표시                           ├─ 후보 안정성 확인
                                                           ├─ affinity/priority 적용
                                                           └─ 원복 및 읽기 검증
```

화면과 게임 목록은 WPF가 맡는다. 반복되는 스레드 관찰과 Windows 스케줄링 API 호출은 C++ DLL 안에서 수행한다. WPF와 C++은 `native/EngineApi.h`에 정의된 작은 C ABI로 연결된다.

후보는 CPU 시간 증가량으로 고른 추정치이며 메인 스레드나 입력 스레드로 인증되지 않는다. 엔진은 약 900ms 간격으로 표본을 모으고, 최고 증가량 후보가 4개 관찰 창과 4초 동안 유지될 때 확정한다.

## 2. 매개변수와 상태의 이동

```csharp
// MainWindow: 사용자가 선택한 게임을 네이티브 엔진에 넘긴다.
GameTarget target = (GameTarget)GameBox.SelectedItem;
NativeEngineSession session = NativeEngineSession.Start(target, out startupStatus);

// WPF UI를 막지 않고 C++ 세션의 상태를 읽는다.
IProgress<SessionStatus> progress = new Progress<SessionStatus>(UpdateStatus);
SessionResult result = await session.WaitForCompletionAsync(progress);
```

```cpp
// C++ ABI가 받은 값으로 세션을 만들고 작업 스레드를 시작한다.
GO_SESSION* session = go_start_session(
    processId, processCreationFileTime, fullPath, &startupStatus);

// 백엔드 세션은 자체 루프와 복구를 소유한다.
while (!stopRequested) {
    ValidateTarget(processHandle, processId, creationTime, path);
    auto observation = tracker.Observe(processId);
    if (observation.confirmed)
        policy.Apply(processHandle, observation.candidate);
    PublishStatus(observation);
    WaitForNextSample(900ms);
}
policy.TryRestore(); // 종료 경로에서 원복하고 읽어서 확인
```

| 데이터 | 전달 경로 | 의미 |
|---|---|---|
| `GameTarget` | 선택 목록 → C# 래퍼 → `go_start_session` | PID, 생성 FILETIME, 선택 가능한 실행 경로 |
| `processCreationFileTime` | C# `ToFileTimeUtc()` → C++ `GetProcessTimes()` | PID가 재사용됐는지 확인하는 프로세스 신원 |
| `fullPath` | 선택한 프로세스 → C++ `QueryFullProcessImageNameW()` | 경로를 읽을 수 있는 경우 같은 실행 파일인지 확인 |
| `GO_SESSION*` | C++ 시작 함수 → C# `IntPtr` 래퍼 | 네이티브 실행 세션의 불투명 핸들 |
| `GO_STATUS` | C++ `go_read_status` → C# 진행 DTO → WPF | 후보·진행·복구 상태와 표시 문구 |
| 중단 요청 | 버튼/창 닫기 → `go_request_stop` → C++ 조건 변수 | 최대 다음 표본 대기까지 기다리지 않고 루프를 깨운다 |
| 복구 요청 | WPF 복구 버튼 → `go_try_restore` → 정책 컨트롤러 | 복구 미확인 상태에서 원복만 다시 시도한다 |

## 3. C# 프론트엔드

### `App`과 XAML

`App.xaml`이 `MainWindow.xaml`을 시작 창으로 열고 공용 브러시를 제공한다. `App.xaml.cs`에는 별도 실행 로직이 없다.

```xml
<Application StartupUri="MainWindow.xaml">
    <!-- App.xaml: 버튼 색과 창 배경 색 등 공용 UI 리소스 -->
</Application>
```

`MainWindow.xaml`은 화면 요소와 이벤트 연결을 선언한다.

```xml
<ComboBox x:Name="GameBox" />        <!-- 실행 중인 게임 선택 -->
<Button x:Name="RunButton" />        <!-- 추적 시작·중단·복구 재시도 -->
<TextBlock x:Name="StatusText" />    <!-- 엔진 상태 -->
<TextBlock x:Name="CandidateText" /><!-- 후보 PID/TID와 확정 여부 -->
```

### `MainWindow` — 사용자 동작 연결

화면 상태만 관리하고 CPU 후보 계산이나 affinity 변경은 하지 않는다.

```csharp
class MainWindow : Window
{
    NativeEngineSession? session; // 현재 네이티브 세션 연결
    Task<SessionResult>? runTask; // 완료를 기다리는 비동기 상태 조회
    bool busy;                    // 중복 실행·목록 갱신 방지
    bool closeWhenStopped;        // 복구가 끝나면 닫을 요청을 기억
    bool allowClose;              // 정상 복구 후 닫기 허용

    RefreshGamesAsync();          // GameTarget 목록 갱신
    Run_Click();                  // 시작·중단·복구 재시도
    UpdateStatus(SessionStatus);  // C++ 상태를 UI 문구로 바꿈
    Window_Closing(...);          // 실행 중단 후 복구 전까지 닫기를 보류
}
```

### `GameTarget` — UI 목록용 게임 정보

`Target`은 선택한 작업 대상이라는 뜻이다. 이 타입은 목록 표시와 C++ 시작 인자 전달용 데이터이며, 실행 중 프로세스 신원 검사는 C++ 엔진이 담당한다.

```csharp
record GameTarget(
    int ProcessId,                   // 선택된 PID
    string Executable,               // 표시할 실행 파일 이름
    string? FullPath,                // 경로를 읽을 수 없으면 null
    string Label,                    // ComboBox에 보이는 제목
    long ProcessCreationFileTime)    // ToFileTimeUtc 형식의 생성 시각
{
    FindVisibleGames();              // 창 핸들과 제목이 있는 프로세스를 열거
}
```

### `NativeEngineSession` — 네이티브 세션 핸들 래퍼

이 클래스는 C++ 엔진 알고리즘을 복제하지 않는다. `IntPtr` 수명, P/Invoke 호출, 상태 폴링을 관리한다.

```csharp
class NativeEngineSession : IDisposable
{
    IntPtr handle;                         // GO_SESSION*를 가리키는 불투명 핸들
    Start(GameTarget, out startupStatus);  // go_start_session 호출
    WaitForCompletionAsync(progress);      // go_wait/go_read_status 반복
    RequestStop();                         // go_request_stop 호출
    TryRestore();                           // go_try_restore 호출
    HasPendingRecovery;                     // 상태 조회 실패도 안전하게 복구 필요로 취급
    Dispose();                              // 복구 미확인 상태가 아닐 때만 세션 파기
}
```

### `NativeEngineApi`와 관리 DTO

`NativeEngineApi`는 C++에서 내보낸 함수만 선언한다. C++ 내부 클래스와 정책은 C#에 노출하지 않는다.

```csharp
static class NativeEngineApi
{
    StartSession(...);   // go_start_session
    ReadStatus(...);     // go_read_status
    RequestStop(...);    // go_request_stop
    Wait(...);           // go_wait
    TryRestore(...);     // go_try_restore
    DestroySession(...); // go_destroy_session
}

struct NativeEngineStatus; // C ABI GO_STATUS와 같은 필드 순서·문자열 크기
record SessionStatus(...); // UI에 진행 중 상태를 전달
record SessionResult(...); // 완료 상태를 MainWindow에 전달
```

## 4. C++ 백엔드

### `native/EngineApi.h` — C ABI 경계

C ABI는 C#이 C++ 클래스 ABI에 직접 의존하지 않도록 한다. 이름의 `go_` 접두사는 GameOptimizer API를 구분한다.

```cpp
struct GO_SESSION; // 구현을 감춘 핸들 타입

struct GO_STATUS {
    uint32_t HasCandidate;      // 후보 정보가 있는가
    uint32_t Confirmed;         // 안정성 조건을 통과했는가
    uint32_t RecoveryRequired;  // 미완료 원복이 있는가
    uint32_t Passed;            // 실행 결과
    uint32_t ProcessId;
    uint32_t ThreadId;
    wchar_t Message[512];       // WPF 표시 문구
};

go_start_session(...);   // 검증 후 세션·작업 스레드 생성
go_read_status(...);     // 현재 GO_STATUS 복사
go_request_stop(...);    // 중단 플래그 설정 및 대기 깨우기
go_wait(...);            // 완료까지 제한 시간 대기
go_try_restore(...);     // 완료된 세션의 복구 재시도
go_destroy_session(...); // 복구 미확인 시 파기를 거부
```

### `GO_SESSION`과 `GO_SESSION_IMPL` — 세션 생명주기

`GO_SESSION`은 ABI에서 보이는 불투명 포인터다. 실제 실행 상태는 내부 `GO_SESSION_IMPL`이 소유한다.

```cpp
struct GO_SESSION_IMPL {
    UniqueHandle process;               // 검증된 게임 프로세스 핸들
    DWORD processId;
    uint64_t processCreationFileTime;
    std::wstring processPath;
    ThreadCandidateTracker tracker;     // CPU 표본과 안정 후보 추적
    ThreadPolicyController policy;     // 설정 적용과 복구 상태
    atomic<bool> stopRequested;         // UI의 중단 신호
    mutex + condition_variable;         // 상태 보호와 빠른 중단 대기
    GO_STATUS status;                   // UI에 전달할 최신 상태
    thread worker;                      // 관찰 루프
};
```

### `ThreadCandidateTracker` — CPU 증가량 후보 선정

`Tracker`는 표본을 누적해서 변화를 찾는다. 스레드 키에는 PID, TID, 생성 FILETIME을 모두 포함해 TID 재사용을 구분한다.

```cpp
class ThreadCandidateTracker {
    unordered_map<ThreadIdentity, uint64_t> previousCpu; // 직전 커널+사용자 CPU 시간
    optional<ThreadIdentity> currentCandidate;           // 현재 1위 후보
    steady_clock::time_point candidateSince;             // 후보 시작 시각
    int candidateWindows;                                // 연속 표본 수

    CandidateObservation Observe(DWORD processId);
    Reset();
};
```

`Observe`는 Toolhelp 스레드 스냅샷과 `GetThreadTimes`를 사용한다. 신원을 확인할 수 없는 스레드가 있으면 표본 전체를 초기화해 불완전한 목록으로 후보를 확정하지 않는다.

### `ThreadPolicyController` — 적용과 원복 검증

기존 그룹에서 허용된 CPU 마스크의 앞 두 비트를 고르고 `THREAD_PRIORITY_ABOVE_NORMAL`을 설정한다.

```cpp
class ThreadPolicyController {
    optional<PendingRestore> pending; // 확인 끝날 때까지 핸들과 원본 값을 보존

    Apply(process, processId, creationTime, path, candidate, stopFlag);
    TryRestore();
    HasPendingRecovery();
    LastError();
};
```

`Apply`는 대상 프로세스와 후보 스레드를 재검증하고 원래 affinity/priority를 저장한다. 적용 후 다시 읽어 확인한다. `TryRestore`는 현재 값이 앱의 적용값인 경우만 원래 값으로 돌린다. 외부 변경값은 덮어쓰지 않는다.

### `PendingRestore` — 복구 대기 데이터

정책 컨트롤러 내부에 있는 상태다. 변경된 설정별로 복구 완료 여부를 따로 저장한다.

```cpp
struct PendingRestore {
    UniqueHandle thread;              // 원래 스레드 커널 객체를 계속 붙잡는다
    ThreadIdentity identity;           // PID·TID·생성 FILETIME
    GROUP_AFFINITY originalAffinity;
    GROUP_AFFINITY appliedAffinity;
    int originalPriority;
    int appliedPriority;
    bool needsAffinityRestore;
    bool needsPriorityRestore;
};
```

### `UniqueHandle`와 Windows API

`UniqueHandle`은 HANDLE의 소유자다. 범위를 벗어날 때 `CloseHandle`을 호출하고 복사 대신 이동만 허용한다. 스냅샷·프로세스·스레드 핸들을 누수 없이 닫기 위한 C++ RAII 타입이다.

```cpp
CreateToolhelp32Snapshot / Thread32First / Thread32Next // 스레드 목록
OpenThread / GetThreadTimes                        // 스레드·CPU 시간
GetThreadGroupAffinity / SetThreadGroupAffinity    // CPU 그룹·마스크
GetThreadPriority / SetThreadPriority              // 우선순위
GetProcessTimes / QueryFullProcessImageNameW       // 프로세스 신원
WaitForSingleObject                                // 스레드 생존 여부
CloseHandle                                        // UniqueHandle 소멸 시 정리
```

## 5. 이름과 경계

- `Engine`: UI가 아닌 작업 로직을 실행하는 네이티브 C++ 라이브러리다.
- `NativeEngineSession`: C#에서 하나의 C++ 엔진 세션을 다루는 얇은 연결 객체다.
- `Tracker`: 반복 표본을 추적한다. `Candidate`: 아직 인증이 아니라 정책 적용을 위한 후보를 뜻한다.
- `Identity`: PID/TID 같은 번호와 생성 시각을 묶어 재사용된 번호를 구분한다.
- `Status`: 진행 중 표시, `Result`: 종료 결과, `PendingRestore`: 완료되지 않은 복구다.
- `go_wait`는 제한 시간 동안 완료를 기다리고, `go_request_stop`은 엔진 루프에 중단을 요청한다. 둘은 서로 다른 API다.

## 6. 구성·빌드 파일

```text
GameOptimizer.V2.sln                  # C++ DLL과 WPF 앱을 함께 빌드
GameOptimizer.V2.csproj               # .NET 8 WPF, x64 앱
native/GameOptimizer.Engine.vcxproj   # x64 C++20 DLL 프로젝트
native/EngineApi.h                    # C ABI 헤더
native/EngineApi.cpp                  # 후보 추적·적용·복구 엔진
```

Visual Studio Developer PowerShell에서 C++ 데스크톱과 .NET 데스크톱 도구를 설치한 뒤 다음과 같이 빌드한다.

```powershell
msbuild .\GameOptimizer.V2.sln -m -p:Configuration=Release -p:Platform="Any CPU"
```

## 7. 안전 경계

- CPU 시간으로 고른 후보는 실제 게임 메인·입력 스레드라는 뜻이 아니다.
- 프로세스와 스레드는 생성 시각까지 비교한다. 경로를 읽을 수 있는 프로세스는 실행 경로도 재검증한다.
- 원래 값과 실제 적용값을 따로 보존하고, 복구할 때 현재 설정과 신원을 확인한다.
- 복구를 확인하지 못하면 C++ 세션을 파기하지 않고, WPF는 새 적용과 창 종료를 막는다.
- 강제 종료나 시스템 충돌 뒤 복구를 보장하는 영구 저널은 없다.
- `.NET` UI는 목록 표시와 사용자 상호작용을 담당한다. 후보 추적과 스케줄링 정책은 C++ 엔진에 둔다.
