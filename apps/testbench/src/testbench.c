/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#include "syscfg/syscfg.h"
#include "sysinit/sysinit.h"
#include "sysflash/sysflash.h"

#include <os/os.h>
#include "bsp/bsp.h"
#include <hal/hal_gpio.h>
#include <hal/hal_flash.h>
#include <console/console.h>
#if MYNEWT_VAL(SHELL_TASK)
#include <shell/shell.h>
#endif
#include <log/log.h>
#include <stats/stats.h>
#include <config/config.h>
#include "flash_map/flash_map.h"
#include <hal/hal_system.h>
#if MYNEWT_VAL(SPLIT_LOADER)
#include "split/split.h"
#endif
#include <newtmgr/newtmgr.h>
#include <bootutil/image.h>
#include <bootutil/bootutil.h>
#include <imgmgr/imgmgr.h>
#include <assert.h>
#include <string.h>
#include <json/json.h>
#include <reboot/log_reboot.h>
#include <os/os_time.h>
#include <id/id.h>
#include <os/os_eventq.h>

#include "testutil/testutil.h"

#if MYNEWT_VAL(CONFIG_NFFS)
#include <fs/fs.h>
#include <nffs/nffs.h>
#include "nffs/nffs_test.h"
#endif /* NFFS */

#if MYNEWT_VAL(CONFIG_FCB)
#include <fcb/fcb.h>
/*#include "../fcb/fcb_test.h"*/
#endif /* FCB */

#include "os/os_test.h"
#include "bootutil/bootutil_test.h"

#include <stddef.h>
#include <config/config_file.h>
#include "mbedtls/mbedtls_test.h"

#if MYNEWT_VAL(RUNTEST_CLI)
#include "runtest/runtest.h"
#endif

#include "testbench.h"

struct os_timeval tv;
struct os_timezone tz;

/* Init all tasks */
volatile int tasks_initialized;
int init_tasks(void);

/* Test Task */
#define TESTTASK_PRIO (1)
#define TESTTASK_STACK_SIZE    OS_STACK_ALIGN(256)
static struct os_task testtask;

/* For LED toggling */
int g_led_pin;

os_stack_t *stack1;
os_stack_t *stack2;
os_stack_t *stack3;
os_stack_t *stack4;

/*
 * Test log cbmem buf
 */
#define MAX_CBMEM_BUF 2048
uint32_t *cbmem_buf;
struct cbmem cbmem;

struct log testlog;
int total_tests;
int total_fails;
int blinky_blink;

int forcefail; /* to optionally force a fail on a tests */

#define BLINKY_DUTYCYCLE_SUCCESS 1
#define BLINKY_DUTYCYCLE_FAIL 16

char buildID[TESTBENCH_BUILDID_SZ];

struct runtest_evq_arg test_event_arg;
char runtest_token[RUNTEST_REQ_SIZE];
static int testbench_runtests(struct os_event *ev);
static void testbench_test_complete();

extern uint32_t stack1_size;
extern uint32_t stack2_size;
extern uint32_t stack3_size;
extern uint32_t stack4_size;

void
testbench_ts_pass(char *msg, int msg_len, void *arg)
{
    TESTBENCH_UPDATE_TOD;
    LOG_INFO(&testlog, LOG_MODULE_TEST, "%s test case %s PASSED %s %s",
             buildID, tu_case_name, msg, runtest_token);
    return;
}

void
testbench_ts_fail(char *msg, int msg_len, void *arg)
{
    TESTBENCH_UPDATE_TOD;
    LOG_INFO(&testlog, LOG_MODULE_TEST, "%s test case %s FAILED %s %s",
             buildID, tu_case_name, msg, runtest_token);
    return;
}

#if 0
void
testbench_tc_pretest(void* arg)
{
    return;
}

void
testbench_tc_postest(void* arg)
{
    return;
}
#endif

void
testbench_test_init()
{
    total_tests = 0;
    total_fails = 0;
    forcefail = 0;
    blinky_blink = BLINKY_DUTYCYCLE_SUCCESS;

    return;
}

static int
testbench_runtests(struct os_event *ev)
{
    struct ts_suite *ts;
    struct runtest_evq_arg *runtest_arg;
    int run_all = 0;

    /*
     * testbench_runtests is normally initiated from newtmanager using the cli
     */
    testbench_test_init();
    if (ev != NULL) {
        runtest_arg = ev->ev_arg;

        ts_config.ts_print_results = 0;
        ts_config.ts_system_assert = 0;

        /*
         * The specified "token" is appended to the end of every log message
         * that is level INFO and above (i.e. not log_debug log messages)
         */
        strcpy(runtest_token, runtest_arg->run_token);

        /*
         * In run_all set, each test suite is executed
         */
        run_all = (strcmp(runtest_arg->run_testname, "all") == 0);

        /*
         * If no testname was provided (e.g., in the case where mgmt protocol
         * did not use the newtmgr application), make the default to run all
         * the tests.
         */
        if (runtest_arg->run_testname[0] == '\0') {
            run_all = 1;
        }

        /*
         * go through entire list of registered test suites
         */
        SLIST_FOREACH(ts, &g_ts_suites, ts_next) {
            if (run_all || !strcmp(runtest_arg->run_testname, ts->ts_name)) {
                ts->ts_test();
                total_tests += tu_case_idx;
                total_fails += tu_case_failed;
            }
        }
    } else {
        /*
         * run all tests if NULL event is passed as an argument (untested)
         */
        SLIST_FOREACH(ts, &g_ts_suites, ts_next) {
            ts->ts_test();
            total_tests += tu_case_idx;
            total_fails += tu_case_failed;
        }
    }
    testbench_test_complete();

    return tu_any_failed;
}

/*
 * Print results - ci gateway is checking this message syntax to
 * determine success or failure
 */
static void
testbench_test_complete()
{
    LOG_INFO(&testlog, LOG_MODULE_TEST,
             "%s TESTBENCH TEST %s - Tests run:%d pass:%d fail:%d %s",
             buildID,
             (total_fails ? "FAILED" : "PASSED"),
             total_tests,
             (total_tests-total_fails),
             total_fails,
             runtest_token);
    return;
}

/*
 * Run the tests
 * If any tests fail, blink the LED BLINKY_DUTYCYCLE_FAIL (16) times a second
 */
static void
testtask_handler(void *arg)
{
    os_gettimeofday(&tv, &tz);

    g_led_pin = LED_BLINK_PIN;
    hal_gpio_init_out(g_led_pin, 1);

    while (1) {
        /*
         * if any test fails, blinky the LED more rapidly to
         * provide visual feedback from physical device.
         */
        if (total_fails) {
            blinky_blink = BLINKY_DUTYCYCLE_FAIL;
        }

        while (1) {
            /* Wait one second */
            os_time_delay(OS_TICKS_PER_SEC / blinky_blink);

            /* Toggle the LED */
            hal_gpio_toggle(g_led_pin);
        }
    }
}

/*
 * init_tasks include test workers 
 */
int
init_tasks(void)
{
    os_stack_t *teststack;

    /*
     * malloc the stacks for the testworker tasks
     */
    stack1 = malloc(sizeof(os_stack_t) * TASK1_STACK_SIZE);
    assert(stack1);
    stack1_size = TASK1_STACK_SIZE;

    stack2 = malloc(sizeof(os_stack_t) * TASK2_STACK_SIZE);
    assert(stack2);
    stack2_size = TASK2_STACK_SIZE;

    stack3 = malloc(sizeof(os_stack_t) * TASK3_STACK_SIZE);
    assert(stack3);
    stack3_size = TASK3_STACK_SIZE;

    stack4 = malloc(sizeof(os_stack_t) * TASK4_STACK_SIZE);
    assert(stack4);
    stack4_size = TASK4_STACK_SIZE;

    teststack = malloc(sizeof(os_stack_t) * TESTTASK_STACK_SIZE);
    assert(teststack);
    os_task_init(&testtask, "testtask", testtask_handler, NULL,
                 TESTTASK_PRIO, OS_WAIT_FOREVER, teststack,
                 TESTTASK_STACK_SIZE);

    tasks_initialized = 1;
    return 0;
}

/*
 * buildID string is prepended to each log message.
 * BUILD_TARGET and BUILD_ID are assume to be initialized by
 * build infrastructure. testbench.h sets default values if not.
 */
void
getBuildID()
{
    sprintf(buildID, "%s Build %s:", BUILD_TARGET, BUILD_ID);
}

/*
 * Test suites must be declared before being referenced.
 * Note that we're not actually declaring the test suites themselves,
 * but rather their helper functions which initialize the appropriate
 * callbacks before executing on target HW.
 */
TEST_SUITE_DECL(testbench_mempool);
TEST_SUITE_DECL(testbench_mutex);
TEST_SUITE_DECL(testbench_sem);
TEST_SUITE_DECL(testbench_json);

/*
 * main()
 * Keep this app simple, just run the tests and then report success or failure.
 * Complexity is pushed down to the individual test suites and component test cases.
 */
int
main(int argc, char **argv)
{
    int rc;

    sysinit();

    getBuildID();

    cbmem_buf = malloc(sizeof(uint32_t) * MAX_CBMEM_BUF);
    cbmem_init(&cbmem, cbmem_buf, MAX_CBMEM_BUF);
    log_register("testlog", &testlog, &log_cbmem_handler, &cbmem, LOG_SYSLEVEL);

    log_reboot(hal_reset_cause());

    conf_load();

    /*
     * Register the tests that can be run by lookup
     * - each test is added to the ts_suites slist
     */
    TEST_SUITE_REGISTER(testbench_mempool);
    TEST_SUITE_REGISTER(testbench_mutex);
    TEST_SUITE_REGISTER(testbench_sem);
    TEST_SUITE_REGISTER(testbench_json);

    rc = init_tasks();

    /*
     * This sets the callback function for the events that are
     * generated from newtmanager.
     */
    run_evcb_set((os_event_fn*) testbench_runtests);

    testbench_test_init(); /* initialize globals include blink duty cycle */

    while (1) {
        os_eventq_run(os_eventq_dflt_get());
    }
    assert(0);

    return rc;
}
