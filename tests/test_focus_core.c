/* Host unit tests for focus_core. Build & run: `make test` */
#include <stdio.h>
#include <stdlib.h>

#include "focus_core.h"

static int g_failed, g_checks;

#define CHECK(cond)                                                       \
    do {                                                                  \
        g_checks++;                                                       \
        if (!(cond)) {                                                    \
            g_failed++;                                                   \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

#define MIN (60u * 1000u)

/* Simulated device: a pose and a clock, stepped in 50 ms ticks. */
typedef struct {
    fc_t fc;
    uint32_t now;
    float ax, ay, az;
    uint32_t ev; /* events accumulated by the last run() */
} sim_t;

static void sim_init(sim_t *s, uint32_t focus_ms, uint32_t break_ms)
{
    fc_config_t cfg;
    fc_default_config(&cfg);
    fc_init(&s->fc, &cfg, focus_ms, break_ms);
    s->now = 1000;
    s->ax = 0; s->ay = 0; s->az = 1; /* face up */
    s->ev = 0;
}

static void up(sim_t *s)     { s->ax = 0;    s->ay = 0; s->az = 1; }
static void down(sim_t *s)   { s->ax = 0;    s->ay = 0; s->az = -1; }
static void tilted(sim_t *s) { s->ax = 0.7f; s->ay = 0; s->az = 0.7f; }

static void run(sim_t *s, uint32_t ms)
{
    s->ev = 0;
    for (uint32_t t = 0; t < ms; t += 50) {
        s->ev |= fc_update(&s->fc, s->now, s->ax, s->ay, s->az);
        s->now += 50;
    }
}

static void test_classify(void)
{
    CHECK(fc_classify_pose(0, 0, 1) == FC_POSE_FACE_UP);
    CHECK(fc_classify_pose(0, 0, -1) == FC_POSE_FACE_DOWN);
    CHECK(fc_classify_pose(1, 0, 0) == FC_POSE_TILTED);
    CHECK(fc_classify_pose(0.5f, 0.2f, 0.84f) == FC_POSE_FACE_UP);
    CHECK(fc_classify_pose(0, 0, 0) == FC_POSE_UNKNOWN); /* free fall */
    CHECK(fc_classify_pose(0, 0, 3) == FC_POSE_UNKNOWN); /* impact    */
}

static void test_happy_path(void)
{
    sim_t s;
    sim_init(&s, 25 * MIN, 5 * MIN);
    run(&s, 2000);
    CHECK(s.fc.state == FC_IDLE);

    down(&s);
    run(&s, 2000);
    CHECK(s.fc.state == FC_FOCUS);
    CHECK(s.ev & FC_EV_STARTED);

    run(&s, 25 * MIN);
    CHECK(s.fc.state == FC_DONE);
    CHECK(s.ev & FC_EV_TARGET_REACHED);
    CHECK(fc_remaining_ms(&s.fc) == 0);

    up(&s);
    run(&s, 2000);
    CHECK(s.ev & FC_EV_RECORD);
    CHECK(s.ev & FC_EV_BREAK_STARTED);
    CHECK(s.fc.last_record.completed);
    CHECK(s.fc.last_record.focused_ms == 25 * MIN);
    CHECK(s.fc.last_record.pauses == 0);
    CHECK(s.fc.state == FC_BREAK);

    run(&s, 5 * MIN);
    CHECK(s.ev & FC_EV_BREAK_OVER);
    CHECK(s.fc.state == FC_IDLE);
}

static void test_short_bump_is_ignored(void)
{
    sim_t s;
    sim_init(&s, 25 * MIN, 5 * MIN);
    down(&s);
    run(&s, 2000);
    CHECK(s.fc.state == FC_FOCUS);

    /* someone bumps the desk / peeks for half a second */
    up(&s);
    run(&s, 500);
    down(&s);
    run(&s, 2000);
    CHECK(s.fc.state == FC_FOCUS);
    CHECK(s.fc.pauses == 0);
}

static void test_pause_resume(void)
{
    sim_t s;
    sim_init(&s, 25 * MIN, 5 * MIN);
    down(&s);
    run(&s, 2000);
    run(&s, 10 * MIN);

    tilted(&s); /* picked up and held in the hand */
    run(&s, 2000);
    CHECK(s.fc.state == FC_PAUSED);
    CHECK(s.ev & FC_EV_PAUSED);

    uint32_t frozen = fc_remaining_ms(&s.fc);
    run(&s, MIN);
    CHECK(fc_remaining_ms(&s.fc) == frozen); /* time does not run while paused */

    down(&s);
    run(&s, 2000);
    CHECK(s.fc.state == FC_FOCUS);
    CHECK(s.ev & FC_EV_RESUMED);
    CHECK(s.fc.pauses == 1);
}

static void test_end_while_paused_records_partial(void)
{
    sim_t s;
    sim_init(&s, 25 * MIN, 5 * MIN);
    down(&s);
    run(&s, 2000);
    run(&s, 10 * MIN);
    up(&s);
    run(&s, 2000);
    CHECK(s.fc.state == FC_PAUSED);

    uint32_t ev = fc_end(&s.fc);
    CHECK(ev & FC_EV_RECORD);
    CHECK(!s.fc.last_record.completed);
    CHECK(s.fc.last_record.focused_ms >= 10 * MIN);
    CHECK(s.fc.state == FC_IDLE);
}

static void test_tiny_session_not_recorded(void)
{
    sim_t s;
    sim_init(&s, 25 * MIN, 5 * MIN);
    down(&s);
    run(&s, 5000); /* changed my mind */
    up(&s);
    run(&s, 2000);
    CHECK(fc_end(&s.fc) == 0);
    CHECK(s.fc.state == FC_IDLE);
}

static void test_pause_timeout_abandons(void)
{
    sim_t s;
    sim_init(&s, 25 * MIN, 5 * MIN);
    down(&s);
    run(&s, 2000);
    run(&s, 3 * MIN);
    up(&s);
    run(&s, 2000);
    run(&s, 5 * MIN);
    CHECK(s.fc.state == FC_IDLE);
    CHECK(s.ev & FC_EV_RECORD);
    CHECK(!s.fc.last_record.completed);
}

static void test_flip_during_break_skips_it(void)
{
    sim_t s;
    sim_init(&s, 1 * MIN, 5 * MIN);
    down(&s);
    run(&s, 2000);
    run(&s, MIN);
    up(&s);
    run(&s, 2000);
    CHECK(s.fc.state == FC_BREAK);
    down(&s);
    run(&s, 2000);
    CHECK(s.fc.state == FC_FOCUS);
    CHECK(s.ev & FC_EV_STARTED);
}

static void test_preset_only_when_idle(void)
{
    sim_t s;
    sim_init(&s, 25 * MIN, 5 * MIN);
    CHECK(fc_set_preset(&s.fc, 50 * MIN, 10 * MIN));
    CHECK(fc_remaining_ms(&s.fc) == 50 * MIN);
    down(&s);
    run(&s, 2000);
    CHECK(!fc_set_preset(&s.fc, 15 * MIN, 3 * MIN));
    CHECK(s.fc.focus_target_ms == 50 * MIN);
}

static void test_boots_face_down_starts_immediately(void)
{
    sim_t s;
    sim_init(&s, 25 * MIN, 5 * MIN);
    down(&s); /* very first sample is face down */
    run(&s, 100);
    CHECK(s.fc.state == FC_FOCUS);
}

static void test_clock_wraparound(void)
{
    sim_t s;
    sim_init(&s, 1 * MIN, 0);
    s.now = 0xFFFFFFFFu - 10 * 1000u; /* uint32 ms counter about to wrap */
    down(&s);
    run(&s, 2000);
    run(&s, MIN);
    CHECK(s.fc.state == FC_DONE);
}

int main(void)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        {"classify", test_classify},
        {"happy_path", test_happy_path},
        {"short_bump_is_ignored", test_short_bump_is_ignored},
        {"pause_resume", test_pause_resume},
        {"end_while_paused_records_partial", test_end_while_paused_records_partial},
        {"tiny_session_not_recorded", test_tiny_session_not_recorded},
        {"pause_timeout_abandons", test_pause_timeout_abandons},
        {"flip_during_break_skips_it", test_flip_during_break_skips_it},
        {"preset_only_when_idle", test_preset_only_when_idle},
        {"boots_face_down_starts_immediately", test_boots_face_down_starts_immediately},
        {"clock_wraparound", test_clock_wraparound},
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = g_failed;
        tests[i].fn();
        printf("%s %s\n", g_failed == before ? "ok  " : "FAIL", tests[i].name);
    }
    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
