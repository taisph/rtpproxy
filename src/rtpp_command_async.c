/*
 * Copyright (c) 2010 Sippy Software, Inc., http://www.sippysoft.com
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/un.h>

#include "rtpp_log.h"
#include "rtpp_defines.h"
#include "rtpp_command.h"
#include "rtpp_command_async.h"
#include "rtpp_math.h"
#include "rtpp_network.h"
#include "rtpp_netio_async.h"
#include "rtpp_util.h"

struct rtpp_cmd_async_cf {
    pthread_t thread_id;
    pthread_cond_t cmd_cond;
    pthread_mutex_t cmd_mutex;
    int clock_tick;
    double tused;
    struct recfilter average_load;
};

static void
process_commands(struct cfg *cf, int controlfd_in, double dtime)
{
    int controlfd, i, rval;
    socklen_t rlen;
    struct sockaddr_un ifsun;
    struct rtpp_command *cmd;

    do {
        if (cf->stable.umode == 0) {
            rlen = sizeof(ifsun);
            controlfd = accept(controlfd_in, sstosa(&ifsun), &rlen);
            if (controlfd == -1) {
                if (errno != EWOULDBLOCK)
                    rtpp_log_ewrite(RTPP_LOG_ERR, cf->stable.glog,
                      "can't accept connection on control socket");
                break;
            }
        } else {
            controlfd = controlfd_in;
        }
        cmd = get_command(cf, controlfd, &rval, dtime);
        if (cmd != NULL) {
            pthread_mutex_lock(&cf->glock);
            i = handle_command(cf, cmd);
            pthread_mutex_unlock(&cf->glock);
            free_command(cmd);
        } else {
            i = -1;
        }
        if (cf->stable.umode == 0) {
            close(controlfd);
        }
    } while (i == 0 || cf->stable.umode == 0);
}

static void
rtpp_cmd_queue_run(void *arg)
{
    struct cfg *cf;
    struct rtpp_cmd_async_cf *cmd_cf;
    struct pollfd pfds[1];
    int i, last_ctick;
    double eptime, sptime, tused;

    cf = (struct cfg *)arg;
    cmd_cf = cf->stable.rtpp_cmd_cf;

    pfds[0].fd = cf->stable.controlfd;
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;

    pthread_mutex_lock(&cmd_cf->cmd_mutex);
    last_ctick = cmd_cf->clock_tick;
    pthread_mutex_unlock(&cmd_cf->cmd_mutex);

    for (;;) {
        pthread_mutex_lock(&cmd_cf->cmd_mutex);
        while (cmd_cf->clock_tick == last_ctick) {
            pthread_cond_wait(&cmd_cf->cmd_cond, &cmd_cf->cmd_mutex);
        }
        last_ctick = cmd_cf->clock_tick;
        tused = cmd_cf->tused;
        pthread_mutex_unlock(&cmd_cf->cmd_mutex);

        sptime = getdtime();

        i = poll(pfds, 1, 0);
        if (i < 0 && errno == EINTR)
            continue;
        if (i > 0 && (pfds[0].revents & POLLIN) != 0) {
            process_commands(cf, pfds[0].fd, sptime);
        }
        rtpp_anetio_pump(cf->stable.rtpp_netio_cf);
        eptime = getdtime();
        pthread_mutex_lock(&cmd_cf->cmd_mutex);
        recfilter_apply(&cmd_cf->average_load, (eptime - sptime + tused) * cf->stable.target_pfreq);
        pthread_mutex_unlock(&cmd_cf->cmd_mutex);
#if RTPP_DEBUG
        if (last_ctick % (unsigned int)cf->stable.target_pfreq == 0 || last_ctick < 1000) {
            rtpp_log_write(RTPP_LOG_DBUG, cf->stable.glog, "rtpp_cmd_queue_run %lld sptime %f eptime %f, CSV: %f,%f,%f,%f,%f", \
              last_ctick, sptime, eptime, (double)last_ctick / cf->stable.target_pfreq, \
              eptime - sptime + tused, eptime, sptime, tused);
            rtpp_log_write(RTPP_LOG_DBUG, cf->stable.glog, "run %lld average load %f, CSV: %f,%f", last_ctick, \
              cmd_cf->average_load.lastval * 100.0, (double)last_ctick / cf->stable.target_pfreq, cmd_cf->average_load.lastval);
        }
#endif
    }
}

double
rtpp_command_async_get_aload(struct rtpp_cmd_async_cf *cmd_cf)
{
    double aload;

    pthread_mutex_lock(&cmd_cf->cmd_mutex);
    aload = cmd_cf->average_load.lastval;
    pthread_mutex_unlock(&cmd_cf->cmd_mutex);

    return (aload);
}

int
rtpp_command_async_wakeup(struct rtpp_cmd_async_cf *cmd_cf, int clock)
{
    int old_clock;

    pthread_mutex_lock(&cmd_cf->cmd_mutex);

    old_clock = cmd_cf->clock_tick;
    cmd_cf->clock_tick = clock;
    cmd_cf->tused = 0.0;

    /* notify worker thread */
    pthread_cond_signal(&cmd_cf->cmd_cond);

    pthread_mutex_unlock(&cmd_cf->cmd_mutex);

    return (old_clock);
}

int
rtpp_command_async_init(struct cfg *cf)
{
    struct rtpp_cmd_async_cf *cmd_cf;

    cmd_cf = malloc(sizeof(*cmd_cf));
    if (cmd_cf == NULL)
        return (-1);

    memset(cmd_cf, '\0', sizeof(*cmd_cf));

    pthread_cond_init(&cmd_cf->cmd_cond, NULL);
    pthread_mutex_init(&cmd_cf->cmd_mutex, NULL);

    recfilter_init(&cmd_cf->average_load, 0.999, 0.0, 1);

    cf->stable.rtpp_cmd_cf = cmd_cf;
    if (pthread_create(&cmd_cf->thread_id, NULL, (void *(*)(void *))&rtpp_cmd_queue_run, cf) != 0) {
        pthread_cond_destroy(&cmd_cf->cmd_cond);
        pthread_mutex_destroy(&cmd_cf->cmd_mutex);
        free(cmd_cf);
        cf->stable.rtpp_cmd_cf = NULL;
        return (-1);
    }

    return (0);
}
