# GameOptimizer v3 코드 가이드

저장소 이름은 `GameOptimizer.V2`지만 현재 코드는 v3(`3.0.0-beta1`)다. 이 문서는 현재 코드의 실제 동작, 구조, 복구 설계, 빌드·테스트·배포 절차를 설명한다.

## 1. 목적

스레드 하나가 CPU 작업 대부분을 차지하는 구형 게임(실측: 지배 스레드가 프로세스 CPU 사이클의 약 98%)을 대상으로 한다. 성능은 그 스레드가 올라간 코어 하나가 결정하므로, 다음 상태를 만든다.

- 지배 스레드는 **가장 빠르고 인터럽트·DPC가 적은 물리 코어**에서 실행하고, 그 물리 코어의 형제 논리 CPU(LP)까지 다른 프로그램에서 비운다.
- 나머지 게임 스레드는 지배 코어와 **같은 L3를 공유하는** 다른 코어의 LP 하나에서 실행한다.
- 측정 단계에서는 게임의 CPU 설정을 바꾸지 않는다.

프레임률은 측정하지 않는다. 프로그램은 CPU 배치를 적용하고 그 결과를 확인할 뿐이며, 성능 향상을 보증하지 않는다.

## 2. 구성

| 계층 | 위치 | 역할 |
|---|---|---|
| 런처 (C++) | `launcher/` | 관리자 권한으로 시작해 `app\GameOptimizer.V2.exe`를 실행하고 종료 |
| 앱 (C# WPF, .NET 10) | 저장소 루트 `*.cs`, `*.xaml` | 화면, 대상 선택, 세션 수명, 복구 감시 프로세스, 진단 로그 파일 관리 |
| 엔진 (C++23 DLL) | `native/` | 측정, 코어 선정, 설정 적용·복구, 복구 저널, 진단 로그 기록 |
| 테스트 (C++ 콘솔) | `native/tests/` | 엔진 단위 테스트 |
| CI | `.github/workflows/engine-tests.yml` | push·PR마다 엔진 테스트 빌드·실행 |

앱과 엔진은 C ABI(`native/EngineApi.h`)와 P/Invoke(`NativeEngine.cs`)로 연결한다.

배포 폴더 구조:

```text
GameOptimizer\
├─ GameOptimizer.exe        런처 (사용자가 실행하는 유일한 파일)
└─ app\
   ├─ GameOptimizer.V2.exe  앱 (자체 포함 .NET 10, win-x64)
   ├─ GameOptimizer.Engine.dll
   └─ .NET 런타임과 WPF 파일
```

## 3. 실행 흐름

### 3.1 시작

1. 런처가 관리자 권한을 요청하고 `app\GameOptimizer.V2.exe`를 실행한 뒤 종료한다. `app` 폴더가 없으면 안내 창을 띄운다.
2. 앱은 시작할 때 이전 실행이 남긴 복구 저널을 확인하고, 소유 프로세스가 더 이상 살아 있지 않은 저널을 먼저 복구한다(5.3절).
3. 보이는 최상위 창 중 제목이 있고 소유 창이 없는 창을 프로세스별로 하나씩 조회한다. 자기 자신과 정보를 읽을 수 없는 프로세스는 제외한다. 대상이 하나면 자동 선택하고, 여러 개면 목록에서 고른다.

### 3.2 세션 시작 (`시작` 버튼)

1. 복구 저널 파일을 만들고, 같은 실행 파일을 `--watchdog` 인자로 숨김 실행해 복구 감시 프로세스를 띄운다. 감시 프로세스가 5초 안에 준비되지 않으면 시작하지 않는다.
2. 진단 로그 파일을 만들고 첫 줄에 빌드 버전과 구성을 기록한다(8절).
3. `go_start_session`이 PID, 생성 시각, 실행 경로로 대상을 다시 확인하고 백그라운드 세션 스레드를 시작한다.

세션 스레드는 `PreparePlan → ApplyProcessPolicy → ObserveLoop` 순서로 진행하고, 끝나면 항상 복구를 시도한다(3.7절).

### 3.3 측정과 코어 선정 (`PreparePlan`, 약 9.5초, 게임 설정 변경 없음)

1. 게임 프로세스의 affinity를 읽고, 프로세스 범위와 다른 affinity를 가진 게임 스레드를 진단 로그에 남긴다.
2. `CpuTopology`가 물리 코어, 형제 LP, L3 묶음과 크기, 효율 등급을 읽는다. CPU 그룹이 하나가 아니면 중단한다. L3 정보가 없으면 전체를 한 묶음으로 본다.
3. 다음 두 측정을 **동시에** 실행하고 9.5초 마감에 맞춰 끝낸다.
   - **카운터** (세션 스레드, `ProcessorCounters`): PDH `Processor Information`의 `% Processor Time`, `% Interrupt Time`, `% DPC Time`을 500ms 간격으로 수집해 LP별로 평균한다. 약 20ms마다 중단 요청과 게임 종료를 확인한다.
   - **속도 프로브** (전용 `std::jthread`, `CoreSpeedProbe`): 게임·시스템·옵티마이저 affinity의 교집합에 있는 LP에 측정 스레드를 고정하고, 20ms 예열 후 80ms 동안 정수 연산 반복 횟수를 잰다. 라운드마다 코어 방문 순서를 한 칸씩 회전하고, 같은 코어의 형제 LP를 번갈아 잰다. 첫 라운드 이후에는 표본 사이에 50ms 쉰다. 코어 속도는 모든 표본의 최댓값이며, 최고 코어 대비 %로 바꿔 쓴다.
4. 카운터의 인터럽트·DPC는 프로브의 사용자 모드 연산에 영향을 받지 않으므로 두 측정을 겹쳐도 된다. `% Processor Time`은 프로브 때문에 오염되므로 선정에 쓰지 않고 로그에만 남긴다.
5. `CoreSelection`이 코어를 고른다(4절). 선정에 실패하면 설정을 바꾸지 않고 세션을 끝낸다.

측정 중 중단하거나 게임이 종료되면, 게임 설정을 바꾸기 전이므로 복구할 것이 없다.

### 3.4 프로세스 설정 적용 (`ApplyProcessPolicy`)

1. `ProcessAffinityController`가 CPU 계획을 검증한다. `Reserved`가 비어 있거나 게임 범위가 `Reserved` 밖에 있으면 거부한다.
2. 모든 프로세스를 순회하며 CPU 범위를 바꾼다.
   - 게임: 지배 LP와 보조 LP 두 곳(`Game`)
   - 접근 가능한 다른 프로세스: 현재 범위에서 `Reserved`를 뺀 범위. 남는 CPU가 없으면 시스템 범위에서 `Reserved`를 뺀 범위
   - 접근 거부 프로세스와 Windows 중요 프로세스: 건너뛰고 개수와 예시 3개를 상태로 전달한다. 이 프로세스들은 예약 CPU에서 계속 실행될 수 있다.
3. 각 변경 전에 원래 값을 복구 저널에 기록하고, 적용 후 다시 읽어 검증한다.
4. 게임 프로세스 우선순위를 `HIGH_PRIORITY_CLASS`로 바꾸고 확인한다.

### 3.5 지배 스레드 관찰과 배치 (`ObserveLoop`, 약 900ms 주기)

매 주기마다 다음을 반복한다.

1. 대상 프로세스의 신원을 다시 확인한다.
2. 프로세스 CPU 범위를 다시 적용한다. 그 사이 새로 시작된 프로세스도 예약 CPU에서 빠진다.
3. `ThreadTracker`가 게임 스레드의 CPU 시간 증가량을 비교한다. 한 스레드가 증가량의 80% 이상을 차지하는 상태가 4번 이상의 관찰 창과 4초 이상 이어지면 지배 스레드 후보로 확정한다. 이는 실제 메인 스레드라는 보증이 아니다.
4. 후보가 확정되면 `ThreadPolicy`가 지배 스레드를 지배 LP에 `THREAD_PRIORITY_ABOVE_NORMAL`로, 나머지 게임 스레드를 보조 LP에 배치한다(기존 우선순위 유지).
5. 후보가 바뀌면 이전 후보의 스레드 설정을 먼저 복구한 뒤 새 후보에 적용한다. 후보 근거가 사라지면 스레드 설정을 복구하고 다시 관찰한다.

### 3.6 사후 확인 (첫 스레드 배치 후 1회, 약 3초)

`ResidualLoad`가 500ms × 6회 카운터를 수집하면서 같은 구간의 지배 스레드 CPU 시간을 읽는다.

- 지배 LP 잔여 부하 = 지배 LP 사용률 − 지배 스레드 사용률
- 형제 LP 사용률 = 지배 코어의 다른 LP 중 최댓값
- 전체 LP의 사용률·인터럽트·DPC를 진단 로그에 남긴다.

둘 중 큰 값이 5%를 넘으면 상태 문구에 경고를 붙인다. 진단 전용이므로 실패해도 세션을 중단하지 않는다.

### 3.7 종료와 복구

1. `중단`·`종료` 버튼이나 창 닫기로 중단을 요청하면 세션 스레드가 반복을 멈춘다.
2. 스레드 배치 → 프로세스 CPU 범위 → 프로세스 우선순위 순서로 복구하고, 다시 읽어 확인한다.
3. 모두 복구되면 복구 저널을 지운다. 하나라도 확인하지 못하면 복구 필요 상태로 남기고, 버튼으로 재시도할 수 있게 한다. 이 상태에서는 창을 닫을 수 없다.

## 4. 코어 선정 규칙 (`CoreSelection`)

입력은 측정값뿐이고 Win32 호출이 없는 순수 함수다.

| 단계 | 규칙 |
|---|---|
| 후보 | 게임과 시스템 범위에 모두 포함된 LP가 있고, 측정값이 유효한 물리 코어 |
| L3 선필터 | 후보가 2개 이상인 L3 묶음의 코어만 남긴다 |
| 지배 코어 | 효율 등급(모든 후보에 등급이 있을 때만) → 상대 속도 → 코어 전체 인터럽트+DPC → 물리 코어 번호 |
| 지배 LP | 코어 안에서 인터럽트+DPC가 허용값 이상 낮은 LP. 비슷하면 번호가 작은 LP |
| 보조 코어 | 지배 코어와 같은 L3의 다른 코어 중 코어 전체 인터럽트+DPC → 물리 코어 번호 |
| 보조 LP | 지배 LP와 같은 규칙 |
| 예약 범위 | `Reserved` = 지배 코어의 모든 LP + 보조 LP |
| 실패 조건 | 같은 L3에 후보가 2개 미만이거나, `Reserved`를 빼면 다른 프로그램이 쓸 CPU가 없을 때 |

각 단계는 정렬이 아니라 **최선값 기준 필터**(`KeepNearBest`)다. 최선값과의 차이가 허용값 이내인 후보만 다음 단계로 넘긴다. 허용 오차가 있는 비교로 정렬하면 strict weak ordering이 깨지기 때문이다. 허용값은 속도 1.0%p, 인터럽트+DPC 0.5%p다(9절).

## 5. 복구 설계

### 5.1 신원 확인
프로세스는 PID와 생성 시각, 스레드는 PID·TID와 생성 시각으로 확인해 번호 재사용을 구분한다.

### 5.2 복구 조건
원래 값과 적용 값을 함께 보관한다. 현재 값이 원래 값이면 이미 복구된 것으로 보고, 적용 값일 때만 원래 값으로 되돌린다. 둘 다 아니면 외부에서 바뀐 것으로 보고 덮어쓰지 않는다.

### 5.3 복구 저널과 감시 프로세스
- 위치: `%LOCALAPPDATA%\GameOptimizer.V2\recovery\{PID}_{생성시각}_{GUID}.log`
- 모든 변경은 **적용 전에** 저널에 한 줄씩 기록하고 디스크까지 플러시한다. 마지막 줄이 기록 중에 잘렸다면 그 변경은 적용되지 않은 것이므로 무시한다.
- 복구는 기록 종류별(스레드 affinity, 스레드 우선순위, 프로세스 affinity, 프로세스 변경에 따른 스레드 affinity, 프로세스 우선순위)로 최신 기록부터 되돌린다.
- 감시 프로세스는 앱을 500ms마다 확인한다. 앱이 종료됐는데 저널이 남아 있으면 엔진의 `go_restore_journal`로 복구한다. 이름 있는 뮤텍스로 동시 복구를 막고, 실패하면 500ms 간격으로 최대 10회 재시도한다.
- 감시 프로세스까지 종료된 경우, 남은 저널은 다음 앱 실행 때 처리한다.

## 6. 오류 처리

엔진의 새 코드는 `Result<T>`(`std::expected<T, EngineError>`, `native/EngineResult.h`)를 반환한다.

| `ErrorKind` | 뜻 |
|---|---|
| `Cancelled` | 사용자 중단. 실패로 취급하지 않는다 |
| `TargetExited` | 측정 중 게임 또는 지배 스레드 종료 |
| `InvalidData` | 입력·측정값 검증 실패 |
| `Win32` | Win32 API 실패. `Code`에 `GetLastError` 값 |
| `Pdh` | PDH 실패. `Code`에 PDH 상태 값 |

현재 경계:
- `Result`를 쓰는 곳: 토폴로지, 카운터, 프로브, 선정, 계획, 잔여 부하, `ProcessAffinityController::Apply`, 세션 단계 함수
- 기존 `bool` + 메시지 방식이 남은 곳: `ProcessPriority`, `ThreadPolicy`, `RecoveryJournal`, 각 컨트롤러의 `TryRestore`
- `ProcessAffinityController::Apply`의 실패는 현재 모두 `InvalidData`로 전달된다. Win32 오류 코드가 필요하면 이 부분을 `Win32` 종류로 바꿔야 한다.

## 7. C ABI

| 함수 | 역할 |
|---|---|
| `go_list_visible_games` | 대상 창 목록 |
| `go_start_session` | 세션 시작 (PID, 생성 시각, 경로 검증 여부, 저널 경로, 진단 로그 경로) |
| `go_read_status` | 진행·적용·복구 상태 조회 |
| `go_request_stop` | 중단 요청 |
| `go_wait` | 제한 시간 안에 세션 완료 대기 |
| `go_try_restore` | 복구 재시도 |
| `go_destroy_session` | 복구가 끝난 세션 해제 |
| `go_restore_journal` | 저널 파일로 복구 (감시 프로세스·시작 시 사용) |

`GO_STATUS`(2,976바이트)는 진행 플래그, PID·TID, 접근 제한으로 건너뛴 프로세스 수와 예시 3개(각 320자), 메시지(512자)를 담는다.

ABI를 바꿀 때 규칙:
- `EngineApi.h`와 `NativeEngine.cs`의 구조체 필드 순서와 크기를 함께 바꾼다.
- `EngineApi.cpp`의 `static_assert(sizeof(GO_STATUS) == …)` 값을 갱신한다.
- 문자열은 UTF-16(`wchar_t`, `CharSet.Unicode`)이다.

## 8. 진단 로그

- 위치: `%LOCALAPPDATA%\GameOptimizer.V2\diagnostics\{PID}_{생성시각}_{yyyyMMdd_HHmmss}.txt`
- UTF-8(BOM 없음), CRLF, 줄마다 `yyyy-MM-ddTHH:mm:ss.fff` 시각
- 최근 20개만 보관한다. 복구 저널과 폴더·확장자가 달라 복구 대상으로 읽히지 않는다.
- 로그를 만들지 못해도 세션은 진행한다.

| 줄 | 기록 주체 | 내용 |
|---|---|---|
| `build version= configuration=` | 앱 | 첫 줄. 버전과 커밋 해시 7자리, Debug/Release |
| `target pid= creation=`, `session path=` | 엔진 | 대상 |
| `game affinity original= system=` | 엔진 | 게임·시스템 마스크 |
| `special thread pid= tid= group= mask=` | 엔진 | 별도 affinity를 가진 게임 스레드 |
| `measure elapsed=` / `measure cancelled` | 엔진 | 측정 시간 / 측정 중 중단 |
| `speed core= best=`, `speed core= lp= value=` | 엔진 | 코어 최댓값, 표본 원본 |
| `core= lpMask= l3= l3bytes= speed= eff=` | 엔진 | 코어별 토폴로지와 상대 속도(%) |
| `counter lp= processor= interrupt= dpc=` | 엔진 | LP별 카운터 평균(%) |
| `selection config speedTolerance= intdpcTolerance=` | 엔진 | 선정에 쓴 허용값 |
| `dominant core= lp= … worker core= lp= … game= reserved=` | 엔진 | 선정 결과 |
| `selection failed kind= code=` | 엔진 | 선정 실패 |
| `residual thread= dominantLp= sibling=`, `residual lp= busy= int= dpc=` | 엔진 | 사후 확인 결과 |
| `residual check failed kind= code=` | 엔진 | 사후 확인 실패 |

속도 프로브의 절대값(반복/ms)은 빌드 구성에 따라 크게 다르다(Debug 빌드가 훨씬 낮다). 로그끼리 비교할 때는 첫 줄의 `configuration`을 확인하고, 선정에 쓰는 상대 속도(%)를 기준으로 본다.

## 9. 튜닝값

`native/TuningConfig.h`에 모은다. 허용값과 잔여 경고 기준은 2026-11-16 실측 로그로 정한 잠정값이다.

| 값 | 현재 | 의미 |
|---|---|---|
| `MeasurementBudget` | 9,500ms | 카운터·프로브 동시 측정 마감 |
| `MeasurementCounterInterval` | 500ms | 카운터 표본 간격 |
| `ProbeWarmup` / `ProbeMeasurement` / `ProbeRest` | 20 / 80 / 50ms | 프로브 슬롯 구성 |
| `SpeedTolerancePercent` | 1.0 | 속도 동점 범위(%p) |
| `InterruptDpcTolerancePercent` | 0.5 | 인터럽트+DPC 동점 범위(%p). 10초 측정에서 한 칸이 약 0.17%p라 약 3칸 |
| `ResidualWarningPercent` | 5.0 | 사후 확인 경고 기준 |
| `ResidualSampleInterval` / `ResidualSampleCount` | 500ms / 6 | 사후 확인 측정 |

다른 파일에 있는 값:
- `ThreadTracker.cpp`: 지배 스레드 기준 80%, 관찰 창 4개, 확정 시간 4초
- `EngineApi.cpp`: 관찰 주기 900ms, 게임 프로세스 우선순위 `HIGH_PRIORITY_CLASS`

## 10. 권한과 한계

- 런처와 앱 모두 `requireAdministrator`로 항상 관리자 권한을 요청한다. 관리자 권한으로 실행되는 게임에도 접근하기 위해서다. 감시 프로세스는 앱의 권한을 물려받는다.
- 관리자 권한이어도 보호 프로세스나 Windows 중요 프로세스는 바꾸지 않는다.
- 지원 범위: Windows 10 이상, 단일 CPU 그룹, 논리 프로세서 64개 이하.
- 시작 후 약 9.5초 측정하고, 첫 스레드 배치 뒤 약 3초 사후 확인을 한다. 속도 프로브는 측정 중 코어 하나씩을 잠깐 사용하므로 게임이 짧게 끊길 수 있다.
- 속도 프로브는 정수 연산 처리량으로 코어를 상대 비교한다. 실제 게임 부하에서의 성능 수치가 아니다.
- 실기 검증은 Ryzen 8코어 16스레드(L3 2묶음) 한 대에서만 했다. 인텔 하이브리드, 단일 L3, SMT 없는 CPU는 아직 실측하지 않았다.

## 11. 코드 위치

**런처**
- `launcher/Launcher.cpp`: `app\GameOptimizer.V2.exe` 실행

**앱**
- `App.xaml.cs`: 감시 프로세스 모드 분기, 시작 시 남은 저널 복구
- `MainWindow.xaml`, `MainWindow.xaml.cs`: 화면, 대상 목록, 버튼 상태(`시작`·`중단`·`종료`), 창 닫기 시 복구 대기
- `GameTarget.cs`: 대상 정보 모델
- `NativeEngine.cs`: 세션 래퍼, 상태 구조체, P/Invoke 선언
- `RecoveryWatchdog.cs`: 저널 파일 생성, 감시 프로세스, 남은 저널 복구
- `DiagnosticLogFiles.cs`: 진단 로그 경로, 첫 줄 빌드 정보, 보관 개수

**엔진**
- `EngineApi.h/.cpp`: C ABI, 세션 단계(`PreparePlan`, `ApplyProcessPolicy`, `ObserveLoop`), 복구 순서, 상태 전달
- `EngineResult.h`: `Result`, `ErrorKind`, 가변 길이 레코드 순회(`ForEachRecord`)
- `EngineInternals.h`: 핸들 RAII, 신원 확인, 공용 헬퍼
- `TuningConfig.h`: 튜닝값
- `GameDiscovery.cpp`: 대상 창 조회
- `CpuTopology.cpp`: 물리 코어·LP·L3·효율 등급
- `ProcessorCounters.cpp`: PDH 카운터 수집
- `CoreSpeedProbe.cpp`: 코어 속도 프로브
- `CoreSelection.cpp`: 코어 선정 규칙(순수 함수)
- `CpuPlanner.cpp`: 측정·선정 흐름과 진단 로그
- `ProcessAffinityController.cpp`: 프로세스 CPU 범위 적용·복구
- `ProcessPriority.cpp`: 게임 프로세스 우선순위 적용·복구
- `ThreadTracker.cpp`: 지배 스레드 후보 관찰
- `ThreadPolicy.cpp`: 게임 스레드 배치·우선순위 적용·복구
- `ResidualLoad.cpp`: 사후 확인
- `RecoveryJournal.cpp`: 복구 저널 기록·복구
- `DiagnosticLog.cpp`: 진단 로그 기록

## 12. 빌드, 테스트, 배포

필요 도구: Visual Studio(C++ 데스크톱 개발, .NET 데스크톱 개발), .NET 10 SDK.

### 12.1 빌드
```powershell
msbuild .\GameOptimizer.V2.sln -m -p:Configuration=Release
```
- 엔진 DLL은 `bin\{구성}\net10.0-windows\`에 만들어지고, 앱 빌드 때 `bin\{구성}\net10.0-windows\win-x64\`로 복사된다.
- 솔루션 단위로 빌드해야 엔진이 앱보다 먼저 빌드된다.

### 12.2 테스트
```powershell
msbuild .\native\tests\GameOptimizer.Engine.Tests.vcxproj -p:Configuration=Release -p:Platform=x64 "-p:SolutionDir=$PWD/"
.\bin\tests\Release\GameOptimizer.Engine.Tests.exe
```
종료 코드 0이면 통과다. GitHub Actions(`engine-tests`)가 push와 PR마다 같은 테스트를 Windows에서 실행한다. `native/tests/fixtures/` 폴더가 있으면 그 경로를 테스트 실행 파일의 인자로 넘긴다.

현재 테스트 범위는 `EngineResult`뿐이다. 선정 규칙(`CoreSelection`)은 실측 로그 기반 테스트를 추가할 예정이다.

### 12.3 배포 패키지
```powershell
Remove-Item .\out\Release -Recurse -Force -ErrorAction SilentlyContinue
msbuild .\GameOptimizer.V2.sln -m -p:Configuration=Release
dotnet publish .\GameOptimizer.V2.csproj -c Release -r win-x64 --self-contained -p:DebugType=none -o .\out\Release\app
```
- `out\Release`에는 런처 `GameOptimizer.exe`와 `app\` 폴더만 있어야 한다.
- `out\Release`의 내용을 `GameOptimizer\` 폴더로 감싸 zip으로 압축한다.
- PDB(앱, 엔진, 런처)는 배포하지 않고 빌드 커밋과 함께 따로 보관한다.
- 배포 전 로컬 커밋을 push한다. 진단 로그 첫 줄의 커밋 해시로 코드를 찾을 수 있어야 한다.
