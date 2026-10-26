# 프로젝트 도구

검증·테스트·빌드·코드 정리·이미지 제작 도구는 이 폴더에 모읍니다.
개발 PC에서는 실행 파일과 Python 패키지, MSVC/SDK까지 프로젝트 아래의 복사본을 사용합니다.
Windows 운영체제 자체의 DLL과 PowerShell은 기본 시스템 구성요소입니다.

## 최초 준비

```powershell
.\tools\setup-local-tools.ps1
```

이 스크립트는 현재 설치된 Python, Node, Git, GitHub CLI, Visual Studio 2026 / Windows SDK에서
필요한 파일을 **데이터로 읽어** 프로젝트에 복사합니다. 설치 원본의 C 드라이브 실행 파일은
호출하지 않습니다. 준비 후 명령은 아래 로컬 경로만 사용합니다.

| 위치 | 용도 |
|---|---|
| `runtime/python` | 독립 Python 실행 환경 |
| `runtime/node` | JavaScript 구문 검사 |
| `runtime/git`, `runtime/gh` | Git 및 GitHub 작업 |
| `runtime/format` | 소스 포맷 정리 |
| `runtime/fonts` | 이미지 제작용 로컬 글꼴 |
| `vendor/python` | Pillow 및 resvg_py |
| `toolchain/msvc`, `toolchain/windows-sdk` | 로컬 컴파일러·헤더·라이브러리·리소스 도구 |
| `cache`, `output` | 임시 파일, 테스트 백업·보고서, 중간 결과 |

위 런타임·패키지·글꼴·툴체인은 로컬 개발 의존성입니다. Git의 로컬 제외 목록에 등록하여
저장소나 릴리스에 업로드하지 않습니다. 특히 설치된 Microsoft 도구와 글꼴을 재배포하지 않습니다.
프로젝트의 제작·수정 스크립트와 테스트 소스는 공개합니다.

## 사용하는 명령

```powershell
# 로컬 MSVC로 dist/ShellTidy.exe 빌드 및 격리 테스트
.\tools\build.ps1 -Test

# 공개된 EXE를 보존하면서 별도 경로에 재빌드
.\tools\build.ps1 -OutputDirectory "$PWD\tools\output\local-build" -Test

# 사이트·공유 DLL 의존성·크기 확인 및 격리 테스트
.\tools\verify.ps1 -Test

# 테스트만 실행 (실제 사용자 등록이나 Explorer를 변경하지 않음)
.\tools\test.ps1

# 아이콘 / 영문 Social Preview / 가상 UI 이미지 재생성
.\tools\render-assets.ps1 -Asset All

# 소스 포맷 / 로컬 Git
.\tools\format.ps1
.\tools\git.ps1 status --short
```

`native_tests.cpp`는 50개 검증을 제공하는 네이티브 자체 테스트 코드입니다.
`ShellTidy.vcxproj`와 `tools/build.ps1`이 함께 컴파일합니다.

`generate_ui_previews.py`는 고정된 가상 앱/경로만 사용하며 사용자 레지스트리나 설치 목록을
읽지 않습니다. `assets/screenshots`는 **예시 데이터로 재구성한 UI 이미지**입니다.
`generate_social_preview.py`는 템플릿 비율 1280×640, 안전 여백 80px의 영문 카드를 생성합니다.

이미지 생성 스크립트는 `render-assets.ps1`을 통해 실행하세요. 이 진입점이 Python 패키지,
Node, 글꼴, 캐시 경로를 `tools` 내부로 고정하며 사용자 C 드라이브 패키지를 참조하지 않습니다.
