/*
 * iperf, Copyright (c) 2014-2023, The Regents of the University of
 * California, through Lawrence Berkeley National Laboratory (subject
 * to receipt of any required approvals from the U.S. Dept. of
 * Energy).  All rights reserved.
 *
 * If you have questions about your rights to use or distribute this
 * software, please contact Berkeley Lab's Technology Transfer
 * Department at TTD@lbl.gov.
 *
 * NOTICE.  This software is owned by the U.S. Department of Energy.
 * As such, the U.S. Government has been granted for itself and others
 * acting on its behalf a paid-up, nonexclusive, irrevocable,
 * worldwide license in the Software to reproduce, prepare derivative
 * works, and perform publicly and display publicly.  Beginning five
 * (5) years after the date permission to assert copyright is obtained
 * from the U.S. Department of Energy, and subject to any subsequent
 * five (5) year renewals, the U.S. Government is granted for itself
 * and others acting on its behalf a paid-up, nonexclusive,
 * irrevocable, worldwide license in the Software to reproduce,
 * prepare derivative works, distribute copies to the public, perform
 * publicly and display publicly, and to permit others to do so.
 *
 * This code is distributed under a BSD style license, see the LICENSE
 * file for complete information.
 */
#include "iperf_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <getopt.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "iperf.h"
#include "iperf_api.h"
#include "iperf_util.h"
#include "iperf_locale.h"
#include "net.h"
#include "units.h"


static int run(struct iperf_test *test);

/* 解析带单位的速率字符串，返回 bit/s
 * 支持格式示例：
 *   "1000000"    -> 1000000 bit/s
 *   "24KB"       -> 24 * 1024 * 8 bit/s
 *   "0.2MB"      -> 0.2 * 1024^2 * 8 bit/s
 *   "12M"        -> 12 * 1024^2 bit/s
 *   "2GB"        -> 2 * 1024^3 * 8 bit/s
 *
 * 单位规则：
 *   [K|M|G][B]   (大小写均可)
 *   默认是 bits/s；带 B / b 时表示 Bytes/s，再 *8 变成 bits/s
 */
static iperf_size_t
parse_rate_with_units(const char* s)
{
    double value;
    char* end;
    double scale = 1.0;
    int is_bytes = 0;

    if (s == NULL || *s == '\0')
        return 0;

    value = strtod(s, &end);
    if (end == s) {
        /* 没有解析出数字 */
        return 0;
    }

    /* 跳过数字后面的空白 */
    while (*end && isspace((unsigned char)*end))
        end++;

    if (*end != '\0') {
        char unit = *end;

        /* 处理 K / M / G 前缀 */
        if (unit == 'k' || unit == 'K' ||
            unit == 'm' || unit == 'M' ||
            unit == 'g' || unit == 'G') {

            switch (unit) {
            case 'k': case 'K':
                scale = 1024.0;
                break;
            case 'm': case 'M':
                scale = 1024.0 * 1024.0;
                break;
            case 'g': case 'G':
                scale = 1024.0 * 1024.0 * 1024.0;
                break;
            }

            end++;

            /* 可选 B / b，表示 Bytes */
            if (*end == 'b' || *end == 'B') {
                is_bytes = 1;
                end++;
            }
        }
        else if (*end == 'b' || *end == 'B') {
            /* 只有 B / b，表示 Bytes */
            is_bytes = 1;
            end++;
        }

        /* 可选 "/s" 或 "ps" 之类，简单跳过 */
        if (*end == '/') {
            end++;
            if (*end == 's' || *end == 'S')
                end++;
        }
        else if (*end == 'p' || *end == 'P') {
            end++;
            if (*end == 's' || *end == 'S')
                end++;
        }

        /* 再跳一次尾部空白 */
        while (*end && isspace((unsigned char)*end))
            end++;

        /* 出现奇怪的多余字符，当作错误 */
        if (*end != '\0') {
            return 0;
        }
    }

    if (is_bytes)
        scale *= 8.0;   /* Bytes -> bits */

    if (value <= 0.0)
        return 0;

    /* 转成 bit/s，四舍五入一下 */
    double bits_per_sec = value * scale;
    if (bits_per_sec <= 0.0)
        return 0;

    return (iperf_size_t)(bits_per_sec + 0.5);
}


static void
parse_rate_sweep_args(struct iperf_test* test, int* argc, char*** argvp)
{
    int i;
    int out = 1;
    char** argv = *argvp;

    if (!test || !test->settings) {
        return;
    }

    test->settings->rate_sweep_enabled = 0;

    for (i = 1; i < *argc; ++i) {
        if (strcmp(argv[i], "--rate-sweep") == 0 && i + 1 < *argc) {
            const char* arg = argv[i + 1];
            char buf[128];
            char* saveptr = NULL;
            char* tok;
            const char* parts[4];
            int n_parts = 0;
            iperf_size_t start = 0, end = 0, step = 0;
            double interval = 0.0;

            if (!arg || strlen(arg) >= sizeof(buf)) {
                fprintf(stderr,
                    "iperf3: bad --rate-sweep value '%s'\n",
                    arg ? arg : "(null)");
                exit(1);
            }

            strcpy(buf, arg);

            /* 按 ':' 拆成 4 段：start:end:step:interval */
            tok = strtok_r(buf, ":", &saveptr);
            while (tok && n_parts < 4) {
                parts[n_parts++] = tok;
                tok = strtok_r(NULL, ":", &saveptr);
            }

            if (n_parts != 4) {
                fprintf(stderr,
                    "iperf3: bad --rate-sweep value '%s'\n"
                    "Expected format: start:end:step:interval\n",
                    arg);
                exit(1);
            }

            /* 前三段是速率，支持带单位；最后一段是秒 */
            start = parse_rate_with_units(parts[0]);
            end = parse_rate_with_units(parts[1]);
            step = parse_rate_with_units(parts[2]);
            interval = atof(parts[3]);

            if (start == 0 || end == 0 || step == 0 || interval <= 0.0 || end < start) {
                fprintf(stderr,
                    "iperf3: bad --rate-sweep value '%s'\n"
                    "  start/end/step: >0, 支持可选单位 K/M/G, KB/MB/GB (bit/s)\n"
                    "  interval      : >0 (seconds)\n",
                    arg);
                exit(1);
            }

            test->settings->rate_sweep_enabled = 1;
            test->settings->rate_sweep_start = start;
            test->settings->rate_sweep_end = end;
            test->settings->rate_sweep_step = step;
            test->settings->rate_sweep_interval = interval;

            /* 同步基础 rate，避免其它地方读 settings->rate 时还是旧值 */
            test->settings->rate = start;

            /* 把 '--rate-sweep' 这对参数从 argv 中“吃掉” */
            i++; /* skip value */
        }
        else {
            argv[out++] = argv[i];
        }
    }

    *argc = out;
    argv[out] = NULL;
}



/**************************************************************************/
int
main(int argc, char **argv)
{
    struct iperf_test *test;

    /*
     * Atomics check. We prefer to have atomic types (which is
     * basically on any compiler supporting C11 or better). If we
     * don't have them, we try to approximate the type we need with a
     * regular integer, but complain if they're not lock-free. We only
     * know how to check this on GCC. GCC on CentOS 7 / RHEL 7 is the
     * targeted use case for these check.
     */
#ifndef HAVE_STDATOMIC_H
#ifdef __GNUC__
    if (! __atomic_always_lock_free (sizeof (u_int64_t), 0)) {
#endif // __GNUC__
        fprintf(stderr, "Warning: Cannot guarantee lock-free operation with 64-bit data types\n");
#ifdef __GNUC__
    }
#endif // __GNUC__
#endif // HAVE_STDATOMIC_H

    // XXX: Setting the process affinity requires root on most systems.
    //      Is this a feature we really need?
#ifdef TEST_PROC_AFFINITY
    /* didn't seem to work.... */
    /*
     * increasing the priority of the process to minimise packet generation
     * delay
     */
    int rc = setpriority(PRIO_PROCESS, 0, -15);

    if (rc < 0) {
        perror("setpriority:");
        fprintf(stderr, "setting priority to valid level\n");
        rc = setpriority(PRIO_PROCESS, 0, 0);
    }

    /* setting the affinity of the process  */
    cpu_set_t cpu_set;
    int affinity = -1;
    int ncores = 1;

    sched_getaffinity(0, sizeof(cpu_set_t), &cpu_set);
    if (errno)
        perror("couldn't get affinity:");

    if ((ncores = sysconf(_SC_NPROCESSORS_CONF)) <= 0)
        err("sysconf: couldn't get _SC_NPROCESSORS_CONF");

    CPU_ZERO(&cpu_set);
    CPU_SET(affinity, &cpu_set);
    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpu_set) != 0)
        err("couldn't change CPU affinity");
#endif

    test = iperf_new_test();
    if (!test)
        iperf_errexit(NULL, "create new test error - %s", iperf_strerror(i_errno));
    iperf_defaults(test);	/* sets defaults */

    parse_rate_sweep_args(test, &argc, &argv);

    if (iperf_parse_arguments(test, argc, argv) < 0) {
        iperf_err(test, "parameter error - %s", iperf_strerror(i_errno));
        fprintf(stderr, "\n");
        usage();
        exit(1);
    }

    if (run(test) < 0)
        iperf_errexit(test, "error - %s", iperf_strerror(i_errno));

    iperf_free_test(test);

    return 0;
}


static jmp_buf sigend_jmp_buf;
static int signed_sig;

static void __attribute__ ((noreturn))
sigend_handler(int sig)
{
    signed_sig = sig;
    longjmp(sigend_jmp_buf, 1);
}

/**************************************************************************/
static int
run(struct iperf_test *test)
{
    /* Termination signals. */
    iperf_catch_sigend(sigend_handler);
    if (setjmp(sigend_jmp_buf))
	iperf_got_sigend(test, signed_sig);

    /* Ignore SIGPIPE to simplify error handling */
    signal(SIGPIPE, SIG_IGN);

    switch (test->role) {
        case 's':
	    if (test->daemon) {
		int rc;
		rc = daemon(1, 0);
		if (rc < 0) {
		    i_errno = IEDAEMON;
		    iperf_errexit(test, "error - %s", iperf_strerror(i_errno));
		}
	    }
	    if (iperf_create_pidfile(test) < 0) {
		i_errno = IEPIDFILE;
		iperf_errexit(test, "error - %s", iperf_strerror(i_errno));
	    }
            for (;;) {
		int rc;
		rc = iperf_run_server(test);
                test->server_last_run_rc = rc;
		if (rc < 0) {
		    iperf_err(test, "error - %s", iperf_strerror(i_errno));
                    if (test->json_output) {
                        if (iperf_json_finish(test) < 0)
                            return -1;
                    }
                    iflush(test);

		    if (rc < -1) {
		        iperf_errexit(test, "exiting");
		    }
                }
                iperf_reset_test(test);
                if (iperf_get_test_one_off(test) && rc != 2) {
		    /* Authentication failure doesn't count for 1-off test */
		    if (rc < 0 && i_errno == IEAUTHTEST) {
			continue;
		    }
		    break;
		}
            }
	    iperf_delete_pidfile(test);
            break;
	case 'c':
	    if (iperf_create_pidfile(test) < 0) {
		i_errno = IEPIDFILE;
		iperf_errexit(test, "error - %s", iperf_strerror(i_errno));
	    }
	    if (iperf_run_client(test) < 0)
		iperf_errexit(test, "error - %s", iperf_strerror(i_errno));
	    iperf_delete_pidfile(test);
            break;
        default:
            usage();
            break;
    }

    iperf_catch_sigend(SIG_DFL);
    signal(SIGPIPE, SIG_DFL);

    return 0;
}
