/* Host unit tests for core/sensors.c. Build & run: `make test` */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "sensors.h"

static int g_failed, g_checks;

#define CHECK(cond)                                                           \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(cond)) {                                                        \
            g_failed++;                                                       \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                     \
    } while (0)

#define NEAR(a, b, tol) (fabsf((float)(a) - (float)(b)) <= (tol))

static const sx_vec3_t UP = {0, 0, 1};
/* Seoul-ish field: ~30 µT horizontal towards north, ~40 µT pointing down */
static const sx_vec3_t FIELD = {30, 0, -40};

static float angle_diff(float a, float b)
{
    float d = fmodf(a - b + 540.0f, 360.0f) - 180.0f;
    return fabsf(d);
}

static void test_tilt(void)
{
    float p, r;
    sx_tilt_deg(UP, &p, &r);
    CHECK(NEAR(p, 0, 0.01f) && NEAR(r, 0, 0.01f));

    for (int pitch = -60; pitch <= 60; pitch += 30) {
        for (int roll = -150; roll <= 150; roll += 50) {
            sx_vec3_t a = sx_world_to_body(123, (float)pitch, (float)roll, UP);
            sx_tilt_deg(a, &p, &r);
            CHECK(NEAR(p, pitch, 0.1f));
            CHECK(NEAR(r, roll, 0.1f));
        }
    }
}

static void test_heading_flat(void)
{
    for (int h = 0; h < 360; h += 15) {
        sx_vec3_t a = sx_world_to_body((float)h, 0, 0, UP);
        sx_vec3_t m = sx_world_to_body((float)h, 0, 0, FIELD);
        CHECK(angle_diff(sx_heading_deg(a, m), (float)h) < 0.5f);
    }
}

static void test_heading_tilt_compensated(void)
{
    /* Same heading, any tilt -> same answer. A naive atan2(my, mx)
     * would be off by tens of degrees here. */
    int worst_naive = 0;
    for (int h = 0; h < 360; h += 45) {
        for (int pitch = -50; pitch <= 50; pitch += 25) {
            for (int roll = -50; roll <= 50; roll += 25) {
                sx_vec3_t a = sx_world_to_body((float)h, (float)pitch, (float)roll, UP);
                sx_vec3_t m = sx_world_to_body((float)h, (float)pitch, (float)roll, FIELD);
                CHECK(angle_diff(sx_heading_deg(a, m), (float)h) < 0.5f);

                float naive = atan2f(m.y, m.x) * 57.29578f;
                if (naive < 0) naive += 360;
                int err = (int)angle_diff(naive, (float)h);
                if (err > worst_naive) worst_naive = err;
            }
        }
    }
    CHECK(worst_naive > 30); /* proves the test actually exercises tilt */
}

static void test_cardinal(void)
{
    CHECK(sx_cardinal(0)[0] == 'N' && sx_cardinal(0)[1] == 0);
    CHECK(sx_cardinal(359)[0] == 'N' && sx_cardinal(359)[1] == 0);
    CHECK(sx_cardinal(45)[0] == 'N' && sx_cardinal(45)[1] == 'E');
    CHECK(sx_cardinal(90)[0] == 'E');
    CHECK(sx_cardinal(200)[0] == 'S' && sx_cardinal(200)[1] == 0); /* S spans 157.5-202.5 */
    CHECK(sx_cardinal(225)[0] == 'S' && sx_cardinal(225)[1] == 'W');
}

static void test_mag_interference(void)
{
    sx_vec3_t a = {30, 0, -40}, b = {31, 1, -40};
    CHECK(sx_mag_disagreement(a, b) < SX_MAG_INTERFERENCE_UT);
    sx_vec3_t near_magnet = {30 + 25, 0, -40};
    CHECK(sx_mag_disagreement(a, near_magnet) > SX_MAG_INTERFERENCE_UT);
}

static void test_shake(void)
{
    sx_shake_t s;
    sx_shake_init(&s);
    uint32_t t = 0;
    int events = 0;

    /* resting: never a shake */
    for (int i = 0; i < 100; i++, t += 50) events += sx_shake_update(&s, UP, t);
    CHECK(events == 0);

    /* one bump (e.g. putting it down hard): still not a shake */
    sx_vec3_t bump = {0, 0, 2.5f};
    events += sx_shake_update(&s, bump, t);
    for (int i = 0; i < 40; i++) events += sx_shake_update(&s, UP, t += 50);
    CHECK(events == 0);

    /* vigorous 5 Hz shaking for 1.2 s: exactly one event (cooldown) */
    for (int i = 0; i < 24; i++, t += 50) {
        float x = 2.2f * sinf(2 * 3.14159265f * 5.0f * (float)i * 0.05f + 0.3f);
        sx_vec3_t a = {x, 0, 1};
        events += sx_shake_update(&s, a, t);
    }
    CHECK(events == 1);
}

static void test_yaw(void)
{
    sx_yaw_t y;
    sx_yaw_init(&y);
    uint32_t t = 0;
    /* 180 deg/s for 4.5 s = 810 deg = 2 full turns */
    for (int i = 0; i <= 90; i++, t += 50) sx_yaw_update(&y, 180, t);
    CHECK(NEAR(y.yaw_deg, 810, 1));
    CHECK(sx_yaw_turns(&y) == 2);

    /* gyro bias below the dead band does not drift */
    float before = y.yaw_deg;
    for (int i = 0; i < 1000; i++, t += 50) sx_yaw_update(&y, 0.8f, t);
    CHECK(NEAR(y.yaw_deg, before, 0.001f));
}

static void test_dbfs(void)
{
    CHECK(NEAR(sx_dbfs(1.0f), 0, 0.01f));
    CHECK(NEAR(sx_dbfs(0.1f), -20, 0.01f));
    CHECK(NEAR(sx_dbfs(0.0f), -90, 0.01f));
    CHECK(sx_sound_class(-70) == SX_SOUND_QUIET);
    CHECK(sx_sound_class(-35) == SX_SOUND_TALK);
    CHECK(sx_sound_class(-10) == SX_SOUND_LOUD);
}

static void test_clap(void)
{
    sx_clap_t c;
    sx_clap_init(&c);
    uint32_t t = 0;
    int claps = 0;

    /* steady conversation level */
    for (int i = 0; i < 100; i++, t += 50) claps += sx_clap_update(&c, -38, t);
    CHECK(claps == 0);

    /* one clap: two loud blocks in a row count once */
    claps += sx_clap_update(&c, -8, t += 50);
    claps += sx_clap_update(&c, -10, t += 50);
    CHECK(claps == 1);
    for (int i = 0; i < 10; i++, t += 50) claps += sx_clap_update(&c, -38, t);

    /* music slowly getting loud: background follows, no clap */
    float db = -38;
    for (int i = 0; i < 400; i++, t += 50) {
        if (db < -12) db += 0.1f;
        claps += sx_clap_update(&c, db, t);
    }
    CHECK(claps == 1);
}

static void test_battery(void)
{
    /* 65 mAh full, screen on (~55 mA): about 70 minutes */
    CHECK(sx_battery_minutes_left(100, 65, 55) == 70);
    /* half charge, face down (~12 mA): 2h 42m */
    CHECK(sx_battery_minutes_left(50, 65, 12) == 162);
    CHECK(sx_battery_minutes_left(80, 65, -150) == UINT32_MAX); /* charging */
}

int main(void)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        {"tilt", test_tilt},
        {"heading_flat", test_heading_flat},
        {"heading_tilt_compensated", test_heading_tilt_compensated},
        {"cardinal", test_cardinal},
        {"mag_interference", test_mag_interference},
        {"shake", test_shake},
        {"yaw", test_yaw},
        {"dbfs", test_dbfs},
        {"clap", test_clap},
        {"battery", test_battery},
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = g_failed;
        tests[i].fn();
        printf("%s %s\n", g_failed == before ? "ok  " : "FAIL", tests[i].name);
    }
    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
