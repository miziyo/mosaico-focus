# Focus Cube for ESP-Mosaico

**엎으면 집중이 시작되는 타이머.** [ESP-Mosaico](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp-mosaico/user_guide.html)(ESP32-S31, 480×480 터치, IMU, 자석 확장 모듈)용 앱이에요. 실물 보드 없이 브라우저 시뮬레이터에서 먼저 개발하고, 나중에 HAL만 갈아 끼워 실기기로 옮기는 구조로 만들었어요.

| 동작 | 결과 |
|---|---|
| 화면이 위로 | 대기. 터치로 15/25/50분 프리셋 선택 |
| **엎어놓기** (1.2초 유지) | 집중 시작. 화면이 꺼지고(배터리 절약), LED 모듈이 빨갛게 켜져 "방해 금지"를 알림 |
| 손에 들기 / 뒤집기 | 일시정지. 다시 엎으면 이어서 진행하고, 5분이 지나면 중단으로 기록 |
| 목표 시간 도달 | 차임이 울리고 LED가 초록으로 바뀜. 들어 올리면 휴식 타이머 시작 |
| 세션 종료 | 완료/중단 기록과 오늘 통계를 저장 (CSV로 내보내기 가능) |

## 구조

```
core/        focus_core.c   자세 판정 + 상태 머신. 순수 C99, 의존성 없음
app/         hal.h          하드웨어 추상화 계층 (IMU, 소리, LED, 백라이트, 저장소)
             focus_app.c    core ↔ UI ↔ HAL 연결
             focus_ui.c     LVGL 9 화면 (480×480)
platform/web/               브라우저 시뮬레이터: Emscripten + JS로 HAL 구현
tests/       test_focus_core.c   코어 단위 테스트 (gcc + ASan/UBSan)
             web_smoke.js        시뮬레이터 E2E 테스트 (Playwright)
```

설계에서 지킨 원칙:
- **로직은 하드웨어를 모른다.** `focus_core`는 가속도 값과 ms 시계만 입력받아요. 그래서 PC에서 25분짜리 시나리오도 몇 ms 안에 테스트할 수 있어요.
- **출력은 상태에서 유도한다.** 화면과 LED를 이벤트마다 켜고 끄지 않고, 현재 상태 하나를 기준으로 결정해요. 그래서 엇갈린 상태가 생기지 않아요.
- **배터리는 65mAh.** 엎어두면 백라이트를 끄고, 내용이 바뀐 위젯만 다시 그려요.

## 실행

```bash
make deps     # LVGL v9.2.2 → third_party/
make test     # 코어 단위 테스트
```

브라우저 시뮬레이터 빌드에는 [Emscripten](https://emscripten.org/docs/getting_started/downloads.html)이 필요해요.

```bash
source /path/to/emsdk/emsdk_env.sh
make serve    # http://localhost:8000
```

시뮬레이터 사용법:
- `Space` 키로 엎기/세우기를 전환해요.
- 시간 배속(최대 300×)을 쓰면 25분 세션을 5초 만에 확인할 수 있어요.
- 폰으로 열고 "폰 기울기 센서 사용"을 켜면, 폰을 실제로 엎어서 테스트할 수 있어요.

## 실기기 포팅 (TODO)

`platform/esp32/`를 만들어 `hal.h`의 함수만 구현하면 돼요. 아직 실물로 검증하지는 않았어요.

| HAL 함수 | ESP-Mosaico 대응 |
|---|---|
| `hal_imu_read` | BMI270 가속도 (BSP 센서 API), ±2g 범위, 20Hz면 충분 |
| `hal_time_ms` | `esp_timer_get_time() / 1000` |
| `hal_day_index` | SNTP 동기화 후 `localtime()` |
| `hal_sound` | ES8311 코덱 + NS4150B 앰프로 짧은 PCM 톤 재생 |
| `hal_status_led` | 인터랙션 모듈의 RGB LED (모듈이 없으면 no-op) |
| `hal_display_set_awake` | 백라이트 off + LVGL 타이머 주기 늘리기, 이후 light sleep 검토 |
| `hal_session_record` / `hal_stats_*` | NVS에 저장, Wi-Fi 연결 시 큐를 비우며 동기화 |

이후 아이디어:
- Notion/Google Calendar로 세션을 동기화해요 (기기 → 작은 릴레이 서버 → API).
- 엎어둔 동안 IMU 인터럽트(wake-on-motion)로 MCU를 재우면 배터리를 훨씬 오래 쓸 수 있어요.
- 주간 통계 화면을 추가해요.
