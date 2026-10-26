# Changelog

## Unreleased

- 영문 Social Preview 카드 SVG/PNG 추가: 1280×640, 안전 여백 80px.
- 소개 페이지에 Open Graph 및 Twitter 카드 이미지 연결.
- README와 소개 페이지에 `context-menus` UI 미리보기 한 장 추가. 앱명과 경로는 가상 데이터이며 실제 사용자 프로그램 목록을 사용하지 않음.
- 검증·테스트·빌드·코드 정리·이미지 생성 도구를 `tools/`로 통합하고 도구 사용법 문서 추가.
- 로컬 Python·Node·Git·GH·MSVC·SDK 및 패키지/글꼴 복사본을 프로젝트 내부 경로에서 실행하도록 구성. 도구 바이너리와 캐시는 공개 소스에서 제외.
- 네이티브 자체 테스트 소스를 `tools/native_tests.cpp`로 이동.
- `.gitignore` 자체 추적 제외 및 로컬 편집기의 프로젝트 Git 경로 정리.

## [0.0.1] - 2026-10-26

첫 공개 릴리스.

- Windows 10/11 x64 대상의 C++20 / Win32 단일 EXE 구현. 정적 CRT(`/MT`) 사용, EXE 700,928 bytes.
- OpenWithList·OpenWithProgids·Applications·파일 연결 참조 및 일반 shell verb·ContextMenuHandlers 검색.
- 실행 경로별 그룹화, 상태/검색 필터, 각 열의 가나다·영문·자연수 오름차순/내림차순 정렬.
- 선택한 등록의 실제 삭제, 삭제 전 영속 백업, 충돌 값을 보존하는 복원 및 MRUList 정합성 유지.
- 현재 기본 앱·Windows 구성요소 보호. UserChoice와 공유 ProgID·CLSID 전체를 삭제하지 않음.
- HKCU 공유 등록을 32/64비트에서 중복 삭제하던 ERROR_RETRY(1237) 수정.
- 작은 단일 창, 화면 중앙 배치, 스크롤 없는 요약 팝업과 상세 복사, 한국어/영어 자동 선택.
- 같은 사용자·세션의 중복 실행 방지 및 기존 창 활성화.
- Explorer 자동 재시작 옵션 제공. 기본 체크 해제, 실제 변경 성공 시만 선택에 따라 재시작.
- Windows 11 최신/패키지 COM 후보 조회 전용 표시.
- MIT 라이선스, 전체 앱 소스·아이콘 원본·Visual Studio 프로젝트·Windows CI 공개.

격리 레지스트리 자동 테스트 50개 통과. 실제 Explorer 재시작, 대화형 UAC, Windows 10 및
전체 DPI별 최종 화면 검수 범위는 [TESTING.md](TESTING.md)에 별도로 기록.

[0.0.1]: https://github.com/deuxdoom/ShellTidy/releases/tag/v0.0.1
