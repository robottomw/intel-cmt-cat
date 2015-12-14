/*
 * BSD LICENSE
 *
 * Copyright(c) 2014-2015 Intel Corporation. All rights reserved.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @brief Platform QoS utility - monitoring module
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>                                      /**< isspace() */
#include <sys/types.h>                                  /**< open() */
#include <sys/stat.h>
#include <sys/ioctl.h>                                  /**< terminal ioctl */
#include <sys/time.h>                                   /**< gettimeofday() */
#include <time.h>                                       /**< localtime() */
#include <fcntl.h>

#include "pqos.h"
#include "main.h"
#include "monitor.h"

#define PQOS_MAX_PIDS         128
#define PQOS_MON_EVENT_ALL    -1

/**
 * Local data structures
 *
 */
static const char *xml_root_open = "<records>";
static const char *xml_root_close = "</records>";
static const char *xml_child_open = "<record>";
static const char *xml_child_close = "</record>";
static const long xml_root_close_size = DIM("</records>") - 1;

/**
 * Number of cores that are selected in config string
 * for monitoring LLC occupancy
 */
static int sel_monitor_num = 0;

/**
 * The mask to tell which events to display
 */
static enum pqos_mon_event sel_events_max = 0;

/**
 * Maintains a table of core, event, number of events that are selected in
 * config string for monitoring LLC occupancy
 */
static struct core_group {
        char *desc;
        int num_cores;
        unsigned *cores;
        struct pqos_mon_data *pgrp;
        enum pqos_mon_event events;
} sel_monitor_core_tab[PQOS_MAX_CORES];

static struct pqos_mon_data *m_mon_grps[PQOS_MAX_CORES];

/**
 * Maintains a table of process id, event, number of events that are selected
 * in config string for monitoring
 */
static struct {
        pid_t pid;
        struct pqos_mon_data *pgrp;
        enum pqos_mon_event events;
} sel_monitor_pid_tab[PQOS_MAX_PIDS];

/**
 * Maintains the number of process id's you want to track
 */
static int sel_process_num = 0;

/**
 * Maintains monitoring interval that is selected in config string for
 * monitoring L3 occupancy
 */
static int sel_mon_interval = 10; /**< 10 = 10x100ms = 1s */

/**
 * Maintains TOP like output that is selected in config string for
 * monitoring L3 occupancy
 */
static int sel_mon_top_like = 0;

/**
 * Maintains monitoring time that is selected in config string for
 * monitoring L3 occupancy
 */
static int sel_timeout = -1;

/**
 * Maintains selected monitoring output file name
 */
static char *sel_output_file = NULL;

/**
 * Maintains selected type of monitoring output file
 */
static char *sel_output_type = NULL;

/**
 * Stop monitoring indicator for infinite monitoring loop
 */
static int stop_monitoring_loop = 0;

/**
 * File descriptor for writing monitored data into
 */
static FILE *fp_monitor = NULL;

/**
 * @brief Check to determine if processes or cores are monitored
 *
 * @return Process monitoring mode status
 * @retval 0 monitoring cores
 * @retval 1 monitoring processes
 */
static inline int
process_mode(void)
{
        return (sel_process_num <= 0) ? 0 : 1;
}

/**
 * @brief Function to safely translate an unsigned int
 *        value to a string
 *
 * @param val value to be translated
 *
 * @return Pointer to allocated string
 */
static char*
uinttostr(const unsigned val)
{
        char buf[16], *str = NULL;

        memset(buf, 0, sizeof(buf));
        snprintf(buf, sizeof(buf), "%u", val);
        selfn_strdup(&str, buf);

        return str;
}

/**
 * @brief Function to set cores group values
 *
 * @param cg pointer to core_group structure
 * @param desc string containing core group description
 * @param cores pointer to table of core values
 * @param num_cores number of cores contained in the table
 *
 * @return Operational status
 * @retval 0 on success
 * @retval -1 on error
 */
static int
set_cgrp(struct core_group *cg, char *desc,
         const uint64_t *cores, const int num_cores)
{
        int i;

        ASSERT(cg != NULL);
        ASSERT(desc != NULL);
        ASSERT(cores != NULL);
        ASSERT(num_cores > 0);

        cg->desc = desc;
        cg->cores = malloc(sizeof(unsigned)*num_cores);
        if (cg->cores == NULL) {
                printf("Error allocating core group table\n");
                return -1;
        }
        cg->num_cores = num_cores;

        /**
         * Transfer cores from buffer to table
         */
        for (i = 0; i < num_cores; i++)
                cg->cores[i] = (unsigned)cores[i];

        return 0;
}

/**
 * @brief Function to set the descriptions and cores for each core group
 *
 * Takes a string containing individual cores and groups of cores and
 * breaks it into substrings which are used to set core group values
 *
 * @param s string containing cores to be divided into substrings
 * @param tab table of core groups to set values in
 * @param max maximum number of core groups allowed
 *
 * @return Number of core groups set up
 * @retval -1 on error
 */
static int
strtocgrps(char *s, struct core_group *tab, const unsigned max)
{
        unsigned i, n, index = 0;
        uint64_t cbuf[PQOS_MAX_CORES];
        char *non_grp = NULL;

        ASSERT(tab != NULL);
        ASSERT(max > 0);

        if (s == NULL)
                return index;

        while ((non_grp = strsep(&s, "[")) != NULL) {
                int ret;
                /**
                 * If group contains single core
                 */
                if ((strlen(non_grp)) > 0) {
                        n = strlisttotab(non_grp, cbuf, (max-index));
                        if ((index+n) > max)
                                return -1;
                        /* set core group info */
                        for (i = 0; i < n; i++) {
                                char *desc = uinttostr((unsigned)cbuf[i]);

                                ret = set_cgrp(&tab[index], desc, &cbuf[i], 1);
                                if (ret < 0)
                                        return -1;
                                index++;
                        }
                }
                /**
                 * If group contains multiple cores
                 */
                char *grp = strsep(&s, "]");

                if (grp != NULL) {
                        char *desc = NULL;

                        selfn_strdup(&desc, grp);
                        n = strlisttotab(grp, cbuf, (max-index));
                        if (index+n > max) {
                                free(desc);
                                return -1;
                        }
                        /* set core group info */
                        ret = set_cgrp(&tab[index], desc, cbuf, n);
                        if (ret < 0) {
                                free(desc);
                                return -1;
                        }
                        index++;
                }
        }

        return index;
}

/**
 * @brief Function to compare cores in 2 core groups
 *
 * This function takes 2 core groups and compares their core values
 *
 * @param cg_a pointer to core group a
 * @param cg_b pointer to core group b
 *
 * @return Whether both groups contain some/none/all of the same cores
 * @retval 1 if both groups contain the same cores
 * @retval 0 if none of their cores match
 * @retval -1 if some but not all cores match
 */
static int
cmp_cgrps(const struct core_group *cg_a,
          const struct core_group *cg_b)
{
        int i, found = 0;

        ASSERT(cg_a != NULL);
        ASSERT(cg_b != NULL);

        const int sz_a = cg_a->num_cores;
        const int sz_b = cg_b->num_cores;
        const unsigned *tab_a = cg_a->cores;
        const unsigned *tab_b = cg_b->cores;

        for (i = 0; i < sz_a; i++) {
                int j;

                for (j = 0; j < sz_b; j++)
                        if (tab_a[i] == tab_b[j])
                                found++;
        }
        /* if no cores are the same */
        if (!found)
                return 0;
        /* if group contains same cores */
        if (sz_a == sz_b && sz_b == found)
                return 1;
        /* if not all cores are the same */
        return -1;
}

/**
 * @brief Common function to parse selected events
 *
 * @param str string of the event
 * @param evt pointer to the selected events so far
 */

static void
parse_event(char *str, enum pqos_mon_event *evt)
{
        ASSERT(str != NULL);
        ASSERT(evt != NULL);
        /**
         * Set event value and sel_event_max which determines
         * what events to display (out of all possible)
         */
        if (strncasecmp(str, "llc:", 4) == 0) {
		*evt = PQOS_MON_EVENT_L3_OCCUP;
                sel_events_max |= *evt;
        } else if (strncasecmp(str, "mbr:", 4) == 0) {
                *evt = PQOS_MON_EVENT_RMEM_BW;
                sel_events_max |= *evt;
        } else if (strncasecmp(str, "mbl:", 4) == 0) {
                *evt = PQOS_MON_EVENT_LMEM_BW;
                sel_events_max |= *evt;
        } else if (strncasecmp(str, "all:", 4) == 0 ||
                   strncasecmp(str, ":", 1) == 0)
                *evt = PQOS_MON_EVENT_ALL;
        else
                parse_error(str, "Unrecognized monitoring event type");
}

/**
 * @brief Verifies and translates monitoring config string into
 *        internal monitoring configuration.
 *
 * @param str string passed to -m command line option
 */
static void
parse_monitor_event(char *str)
{
        int i = 0, n = 0;
        enum pqos_mon_event evt = 0;
        struct core_group cgrp_tab[PQOS_MAX_CORES];

        parse_event(str, &evt);

        n = strtocgrps(strchr(str, ':') + 1,
                       cgrp_tab, PQOS_MAX_CORES);
        if (n < 0) {
                printf("Error: Too many cores selected\n");
                exit(EXIT_FAILURE);
        }
        /**
         *  For each core group we are processing:
         *  - if it's already in the sel_monitor_core_tab
         *    =>  update the entry
         *  - else
         *    => add it to the sel_monitor_core_tab
         */
        for (i = 0; i < n; i++) {
                int j, found;

                for (found = 0, j = 0; j < sel_monitor_num && found == 0; j++) {
                        found = cmp_cgrps(&sel_monitor_core_tab[j],
                                          &cgrp_tab[i]);
                        if (found < 0) {
                                printf("Error: cannot monitor same "
                                       "cores in different groups\n");
                                exit(EXIT_FAILURE);
                        }
                        if (found)
                                sel_monitor_core_tab[j].events |= evt;
                }
                if (!found) {
                        sel_monitor_core_tab[sel_monitor_num] = cgrp_tab[i];
                        sel_monitor_core_tab[sel_monitor_num].events = evt;
			m_mon_grps[sel_monitor_num] =
                                malloc(sizeof(**m_mon_grps));
                        if (m_mon_grps[sel_monitor_num] == NULL) {
                                printf("Error with memory allocation");
                                exit(EXIT_FAILURE);
                        }
                        sel_monitor_core_tab[sel_monitor_num].pgrp =
                                m_mon_grps[sel_monitor_num];
                        ++sel_monitor_num;
                }
        }
        return;
}

void selfn_monitor_file_type(const char *arg)
{
        selfn_strdup(&sel_output_type, arg);
}

void selfn_monitor_file(const char *arg)
{
        selfn_strdup(&sel_output_file, arg);
}

void selfn_monitor_cores(const char *arg)
{
        char *cp = NULL, *str = NULL;
        char *saveptr = NULL;

        if (arg == NULL)
                parse_error(arg, "NULL pointer!");

        if (strlen(arg) <= 0)
                parse_error(arg, "Empty string!");
        /**
         * The parser will add to the display only necessary columns
         */
        sel_events_max = 0;

        selfn_strdup(&cp, arg);

        for (str = cp; ; str = NULL) {
                char *token = NULL;

                token = strtok_r(str, ";", &saveptr);
                if (token == NULL)
                        break;
                parse_monitor_event(token);
        }

        free(cp);
}

int monitor_setup(const struct pqos_cpuinfo *cpu_info,
                  const struct pqos_capability const *cap_mon)
{
        unsigned i;
        int ret;
        enum pqos_mon_event all_core_evts = 0, all_pid_evts = 0;
        const enum pqos_mon_event evt_all =
                (enum pqos_mon_event)PQOS_MON_EVENT_ALL;

        ASSERT(sel_monitor_num >= 0);
        ASSERT(sel_process_num >= 0);

        /**
         * Check output file type
         */
        if (sel_output_type == NULL)
                sel_output_type = strdup("text");

        if (sel_output_type == NULL) {
                printf("Memory allocation error!\n");
                return -1;
        }

        if (strcasecmp(sel_output_type, "text") != 0 &&
            strcasecmp(sel_output_type, "xml") != 0 &&
            strcasecmp(sel_output_type, "csv") != 0) {
                printf("Invalid selection of file output type'%s'!\n",
                       sel_output_type);
                return -1;
        }

        /**
         * Set up file descriptor for monitored data
         */
        if (sel_output_file == NULL) {
                fp_monitor = stdout;
        } else {
                if (strcasecmp(sel_output_type, "xml") == 0 ||
                    strcasecmp(sel_output_type, "csv") == 0)
                        fp_monitor = fopen(sel_output_file, "w+");
                else
                        fp_monitor = fopen(sel_output_file, "a");
                if (fp_monitor == NULL) {
                        perror("Monitoring output file open error:");
                        printf("Error opening '%s' output file!\n",
                               sel_output_file);
                        return -1;
                }
                if (strcasecmp(sel_output_type, "xml") == 0) {
                        if (fseek(fp_monitor, 0, SEEK_END) == -1) {
                                perror("File seek error");
                                return -1;
                        }
                        if (ftell(fp_monitor) == 0)
                                fprintf(fp_monitor,
                                        "<?xml version=\"1.0\" encoding="
                                        "\"UTF-8\"?>\n%s\n", xml_root_open);
                }
        }

        /**
         * get all available events on this platform
         */
        for (i = 0; i < cap_mon->u.mon->num_events; i++) {
                struct pqos_monitor *mon = &cap_mon->u.mon->events[i];

                all_core_evts |= mon->type;
                if (mon->pid_support)
                        all_pid_evts |= mon->type;
        }
        /**
         * If no cores and events selected through command line
         * by default let's monitor all cores
         */
        if (sel_monitor_num == 0 && sel_process_num == 0) {
	        sel_events_max = all_core_evts;
                for (i = 0; i < cpu_info->num_cores; i++) {
                        unsigned lcore  = cpu_info->cores[i].lcore;
                        uint64_t core = (uint64_t)lcore;

                        ret = set_cgrp(&sel_monitor_core_tab[sel_monitor_num],
                                       uinttostr(lcore), &core, 1);
                        sel_monitor_core_tab[sel_monitor_num].events =
			        sel_events_max;
			m_mon_grps[sel_monitor_num] =
			        malloc(sizeof(**m_mon_grps));
			if (m_mon_grps[sel_monitor_num] == NULL) {
			        printf("Error with memory allocation");
				exit(EXIT_FAILURE);
			}
			sel_monitor_core_tab[sel_monitor_num].pgrp =
			        m_mon_grps[sel_monitor_num];
                        sel_monitor_num++;
                }
        }

	if (sel_process_num > 0 && sel_monitor_num > 0) {
		printf("Monitoring start error, process and core"
		       " tracking can not be done simultaneously\n");
		return -1;
	}

	if (!process_mode()) {
                /**
                 * Make calls to pqos_mon_start - track cores
                 */
                for (i = 0; i < (unsigned)sel_monitor_num; i++) {
                        struct core_group *cg = &sel_monitor_core_tab[i];

                        /* check if all available events were selected */
                        if (cg->events == evt_all) {
                                cg->events = all_core_evts;
                                sel_events_max |= all_core_evts;
                        }
                        ret = pqos_mon_start(cg->num_cores, cg->cores,
                                             cg->events, (void *)cg->desc,
                                             cg->pgrp);
                        ASSERT(ret == PQOS_RETVAL_OK);
                        /**
                         * The error raised also if two instances of PQoS
                         * attempt to use the same core id.
                         */
                        if (ret != PQOS_RETVAL_OK) {
                                printf("Monitoring start error on core(s) "
                                       "%s, status %d\n",
                                       cg->desc, ret);
                                return -1;
                        }
                }
	} else {
                /**
                 * Make calls to pqos_mon_start_pid - track PIDs
                 */
                for (i = 0; i < (unsigned)sel_process_num; i++) {
                        /* check if all available events were selected */
                        if (sel_monitor_pid_tab[i].events == evt_all) {
                                sel_monitor_pid_tab[i].events = all_pid_evts;
                                sel_events_max |= all_pid_evts;
                        }
                        ret = pqos_mon_start_pid(sel_monitor_pid_tab[i].pid,
                                                 sel_monitor_pid_tab[i].events,
                                                 NULL,
                                                 sel_monitor_pid_tab[i].pgrp);
                        ASSERT(ret == PQOS_RETVAL_OK);
                        /**
                         * Any problem with monitoring the process?
                         */
                        if (ret != PQOS_RETVAL_OK) {
                                printf("PID %d monitoring start error,"
                                       "status %d\n",
                                       sel_monitor_pid_tab[i].pid, ret);
                                return -1;
                        }
                }
	}
        return 0;
}

void monitor_stop(void)
{
        unsigned i, mon_number;
        int ret;

	if (!process_mode())
	        mon_number = (unsigned)sel_monitor_num;
	else
	        mon_number = (unsigned)sel_process_num;

	for (i = 0; i < mon_number; i++) {
	        ret = pqos_mon_stop(m_mon_grps[i]);
		ASSERT(ret == PQOS_RETVAL_OK);
		if (ret != PQOS_RETVAL_OK)
		        printf("Monitoring stop error!\n");
	}
        if (!process_mode()) {
                for (i = 0; (int)i < sel_monitor_num; i++) {
                        free(sel_monitor_core_tab[i].desc);
                        free(sel_monitor_core_tab[i].cores);
                }
        }
}

void selfn_monitor_time(const char *arg)
{
        if (!strcasecmp(arg, "inf") || !strcasecmp(arg, "infinite"))
                sel_timeout = -1; /**< infinite timeout */
        else
                sel_timeout = (int) strtouint64(arg);
}

void selfn_monitor_interval(const char *arg)
{
        sel_mon_interval = (int) strtouint64(arg);
}

void selfn_monitor_top_like(const char *arg)
{
        UNUSED_ARG(arg);
        sel_mon_top_like = 1;
}

/**
 * @brief Stores the process id's given in a table for future use
 *
 * @param str string of process id's
 */
static void
sel_store_process_id(char *str)
{
        uint64_t processes[PQOS_MAX_PIDS];
        unsigned i = 0, n = 0;
        enum pqos_mon_event evt = 0;

	parse_event(str, &evt);

        n = strlisttotab(strchr(str, ':') + 1, processes, DIM(processes));

        if (n == 0)
                parse_error(str, "No process id selected for monitoring");

        if (n >= DIM(sel_monitor_pid_tab))
                parse_error(str,
                            "too many processes selected "
                            "for monitoring");

        /**
         *  For each process:
         *  - if it's already there in the sel_monitor_pid_tab
         *  - update the entry
         *  - else - add it to the sel_monitor_pid_tab
         */
        for (i = 0; i < n; i++) {
                unsigned found;
                int j;

                for (found = 0, j = 0; j < sel_process_num && found == 0; j++) {
                        if ((unsigned) sel_monitor_pid_tab[j].pid
			    == processes[i]) {
                                sel_monitor_pid_tab[j].events |= evt;
                                found = 1;
                        }
                }
		if (!found) {
		        sel_monitor_pid_tab[sel_process_num].pid =
			        (pid_t) processes[i];
			sel_monitor_pid_tab[sel_process_num].events = evt;
			m_mon_grps[sel_process_num] =
			        malloc(sizeof(**m_mon_grps));
			if (m_mon_grps[sel_process_num] == NULL) {
			        printf("Error with memory allocation");
			        exit(EXIT_FAILURE);
			}
			sel_monitor_pid_tab[sel_process_num].pgrp =
			        m_mon_grps[sel_process_num];
			++sel_process_num;
		}
        }
}

/**
 * @brief Verifies and translates multiple monitoring config strings into
 *        internal PID monitoring configuration
 *
 * @param arg argument passed to -p command line option
 */
void
selfn_monitor_pids(const char *arg)
{
        char *cp = NULL, *str = NULL;
	char *saveptr = NULL;

        if (arg == NULL)
                parse_error(arg, "NULL pointer!");

        if (strlen(arg) <= 0)
                parse_error(arg, "Empty string!");

        /**
         * The parser will add to the display only necessary columns
         */
        selfn_strdup(&cp, arg);
	sel_events_max = 0;

        for (str = cp; ; str = NULL) {
                char *token = NULL;

                token = strtok_r(str, ";", &saveptr);
                if (token == NULL)
                        break;
                sel_store_process_id(token);
        }

        free(cp);
}

/**
 * @brief Compare LLC occupancy in two monitoring data sets
 *
 * @param a monitoring data A
 * @param b monitoring data B
 *
 * @return LLC monitoring data compare status for descending order
 * @retval 0 if \a  = \b
 * @retval >0 if \b > \a
 * @retval <0 if \b < \a
 */
static int
mon_qsort_llc_cmp_desc(const void *a, const void *b)
{
        const struct pqos_mon_data *const *app =
                (const struct pqos_mon_data * const *)a;
        const struct pqos_mon_data *const *bpp =
                (const struct pqos_mon_data * const *)b;
        const struct pqos_mon_data *ap = *app;
        const struct pqos_mon_data *bp = *bpp;
        /**
         * This (b-a) is to get descending order
         * otherwise it would be (a-b)
         */
        return (int) (((int64_t)bp->values.llc) - ((int64_t)ap->values.llc));
}

/**
 * @brief Compare core id in two monitoring data sets
 *
 * @param a monitoring data A
 * @param b monitoring data B
 *
 * @return Core id compare status for ascending order
 * @retval 0 if \a  = \b
 * @retval >0 if \b > \a
 * @retval <0 if \b < \a
 */
static int
mon_qsort_coreid_cmp_asc(const void *a, const void *b)
{
        const struct pqos_mon_data * const *app =
                (const struct pqos_mon_data * const *)a;
        const struct pqos_mon_data * const *bpp =
                (const struct pqos_mon_data * const *)b;
        const struct pqos_mon_data *ap = *app;
        const struct pqos_mon_data *bp = *bpp;
        /**
         * This (a-b) is to get ascending order
         * otherwise it would be (b-a)
         */
        return (int) (((unsigned)ap->cores[0]) - ((unsigned)bp->cores[0]));
}

/**
 * @brief CTRL-C handler for infinite monitoring loop
 *
 * @param signo signal number
 */
static void monitoring_ctrlc(int signo)
{
        UNUSED_ARG(signo);
        stop_monitoring_loop = 1;
}

/**
 * @brief Gets scale factors to display events data
 *
 * LLC factor is scaled to kilobytes (1024 bytes = 1KB)
 * MBM factors are scaled to megabytes / s (1024x1024 bytes = 1MB)
 *
 * @param cap capability structure
 * @param llc_factor cache occupancy monitoring data
 * @param mbr_factor remote memory bandwidth monitoring data
 * @param mbl_factor local memory bandwidth monitoring data
 * @return operation status
 * @retval PQOS_RETVAL_OK on success
 */
static int
get_event_factors(const struct pqos_cap * const cap,
                  double * const llc_factor,
                  double * const mbr_factor,
                  double * const mbl_factor)
{
        const struct pqos_monitor *l3mon = NULL, *mbr_mon = NULL,
                *mbl_mon = NULL;
        int ret = PQOS_RETVAL_OK;

        if ((cap == NULL) || (llc_factor == NULL) ||
            (mbr_factor == NULL) || (mbl_factor == NULL))
                return PQOS_RETVAL_PARAM;

        if (sel_events_max & PQOS_MON_EVENT_L3_OCCUP) {
                ret = pqos_cap_get_event(cap, PQOS_MON_EVENT_L3_OCCUP, &l3mon);
                if (ret != PQOS_RETVAL_OK) {
                        printf("Failed to obtain LLC occupancy event data!\n");
                        return PQOS_RETVAL_ERROR;
                }
                *llc_factor = ((double)l3mon->scale_factor) / 1024.0;
        } else {
                *llc_factor = 1.0;
        }

        if (sel_events_max & PQOS_MON_EVENT_RMEM_BW) {
                ret = pqos_cap_get_event(cap, PQOS_MON_EVENT_RMEM_BW, &mbr_mon);
                if (ret != PQOS_RETVAL_OK) {
                        printf("Failed to obtain MBR event data!\n");
                        return PQOS_RETVAL_ERROR;
                }
                *mbr_factor = ((double) mbr_mon->scale_factor) /
                        (1024.0*1024.0);
        } else {
                *mbr_factor = 1.0;
        }

        if (sel_events_max & PQOS_MON_EVENT_LMEM_BW) {
                ret = pqos_cap_get_event(cap, PQOS_MON_EVENT_LMEM_BW, &mbl_mon);
                if (ret != PQOS_RETVAL_OK) {
                        printf("Failed to obtain MBL occupancy event data!\n");
                        return PQOS_RETVAL_ERROR;
                }
                *mbl_factor = ((double)mbl_mon->scale_factor) /
                        (1024.0*1024.0);
        } else {
                *mbl_factor = 1.0;
        }

        return PQOS_RETVAL_OK;
}

/**
 * @brief Fills in single text column in the monitoring table
 *
 * @param val numerical value to be put into the column
 * @param data place to put formatted column into
 * @param sz_data available size for the column
 * @param is_monitored if true then \a val holds valid data
 * @param is_column_present if true then corresponding event is
 *        selected for display
 * @return Number of characters added to \a data excluding NULL
 */
static size_t
fillin_text_column(const double val, char data[], const size_t sz_data,
                   const int is_monitored, const int is_column_present)
{
        const char blank_column[] = "           ";
        size_t offset = 0;

        if (sz_data <= sizeof(blank_column))
                return 0;

        if (is_monitored) {
                /**
                 * This is monitored and we have the data
                 */
                snprintf(data, sz_data - 1, "%11.1f", val);
                offset = strlen(data);
        } else if (is_column_present) {
                /**
                 * The column exists though there's no data
                 */
                strncpy(data, blank_column, sz_data - 1);
                offset = strlen(data);
        }

        return offset;
}

/**
 * @brief Fills in text row in the monitoring table
 *
 * @param data the table to store the data row
 * @param sz_data the size of the table
 * @param mon_event events selected in monitoring group
 * @param llc LLC occupancy data
 * @param mbr remote memory bandwidth data
 * @param mbl local memory bandwidth data
 */
static void
fillin_text_row(char data[], const size_t sz_data,
                const enum pqos_mon_event mon_event,
                const double llc, const double mbr,
                const double mbl)
{
        size_t offset = 0;

        ASSERT(sz_data >= 64);

        memset(data, 0, sz_data);

        offset += fillin_text_column(llc, data + offset, sz_data - offset,
                                     mon_event & PQOS_MON_EVENT_L3_OCCUP,
                                     sel_events_max & PQOS_MON_EVENT_L3_OCCUP);

        offset += fillin_text_column(mbl, data + offset, sz_data - offset,
                                     mon_event & PQOS_MON_EVENT_LMEM_BW,
                                     sel_events_max & PQOS_MON_EVENT_LMEM_BW);

        offset += fillin_text_column(mbr, data + offset, sz_data - offset,
                                     mon_event & PQOS_MON_EVENT_RMEM_BW,
                                     sel_events_max & PQOS_MON_EVENT_RMEM_BW);
}

/**
 * @brief Fills in single XML column in the monitoring table
 *
 * @param val numerical value to be put into the column
 * @param data place to put formatted column into
 * @param sz_data available size for the column
 * @param is_monitored if true then \a val holds valid data
 * @param is_column_present if true then corresponding event is
 *        selected for display
 * @param node_name defines XML node name for the column
 * @return Number of characters added to \a data excluding NULL
 */
static size_t
fillin_xml_column(const double val, char data[], const size_t sz_data,
                  const int is_monitored, const int is_column_present,
                  const char node_name[])
{
        size_t offset = 0;

        if (is_monitored) {
                /**
                 * This is monitored and we have the data
                 */
                snprintf(data, sz_data - 1, "\t<%s>%.1f</%s>\n",
                         node_name, val, node_name);
                offset = strlen(data);
        } else if (is_column_present) {
                /**
                 * The column exists though there's no data
                 */
                snprintf(data, sz_data - 1, "\t<%s></%s>\n",
                         node_name, node_name);
                offset = strlen(data);
        }

        return offset;
}

/**
 * @brief Fills in the row in the XML file with the monitoring data
 *
 * @param data the table to store the data row
 * @param sz_data the size of the table
 * @param mon_event events selected in monitoring group
 * @param llc LLC occupancy data
 * @param mbr remote memory bandwidth data
 * @param mbl local memory bandwidth data
 */
static void
fillin_xml_row(char data[], const size_t sz_data,
               const enum pqos_mon_event mon_event,
               const double llc, const double mbr,
               const double mbl)
{
        size_t offset = 0;

        ASSERT(sz_data >= 128);

        memset(data, 0, sz_data);

        offset += fillin_xml_column(llc, data + offset, sz_data - offset,
                                    mon_event & PQOS_MON_EVENT_L3_OCCUP,
                                    sel_events_max & PQOS_MON_EVENT_L3_OCCUP,
                                    "l3_occupancy_kB");

        offset += fillin_xml_column(mbl, data + offset, sz_data - offset,
                                    mon_event & PQOS_MON_EVENT_LMEM_BW,
                                    sel_events_max & PQOS_MON_EVENT_LMEM_BW,
                                    "mbm_local_MB");

        offset += fillin_xml_column(mbr, data + offset, sz_data - offset,
                                    mon_event & PQOS_MON_EVENT_RMEM_BW,
                                    sel_events_max & PQOS_MON_EVENT_RMEM_BW,
                                    "mbm_remote_MB");
}
/**
 * @brief Fills in single CSV column in the monitoring table
 *
 * @param val numerical value to be put into the column
 * @param data place to put formatted column into
 * @param sz_data available size for the column
 * @param is_monitored if true then \a val holds valid data
 * @param is_column_present if true then corresponding event is
 *        selected for display
 * @return Number of characters added to \a data excluding NULL
 */
static size_t
fillin_csv_column(const double val, char data[], const size_t sz_data,
                  const int is_monitored, const int is_column_present)
{
        size_t offset = 0;

        if (is_monitored) {
                /**
                 * This is monitored and we have the data
                 */
                snprintf(data, sz_data - 1, ",%.1f", val);
                offset = strlen(data);
        } else if (is_column_present) {
                /**
                 * The column exists though there's no data
                 */
                snprintf(data, sz_data - 1, ",");
                offset = strlen(data);
        }

        return offset;
}

/**
 * @brief Fills in the row in the CSV file with the monitoring data
 *
 * @param data the table to store the data row
 * @param sz_data the size of the table
 * @param mon_event events selected in monitoring group
 * @param llc LLC occupancy data
 * @param mbr remote memory bandwidth data
 * @param mbl local memory bandwidth data
 */
static void
fillin_csv_row(char data[], const size_t sz_data,
               const enum pqos_mon_event mon_event,
               const double llc, const double mbr,
               const double mbl)
{
        size_t offset = 0;

        ASSERT(sz_data >= 128);

        memset(data, 0, sz_data);

        offset += fillin_csv_column(llc, data + offset, sz_data - offset,
                                    mon_event & PQOS_MON_EVENT_L3_OCCUP,
                                    sel_events_max & PQOS_MON_EVENT_L3_OCCUP);

        offset += fillin_csv_column(mbl, data + offset, sz_data - offset,
                                    mon_event & PQOS_MON_EVENT_LMEM_BW,
                                    sel_events_max & PQOS_MON_EVENT_LMEM_BW);

        offset += fillin_csv_column(mbr, data + offset, sz_data - offset,
                                    mon_event & PQOS_MON_EVENT_RMEM_BW,
                                    sel_events_max & PQOS_MON_EVENT_RMEM_BW);
}

/**
 * @brief Prints row of monitoring data in text format
 *
 * @param fp pointer to file to direct output
 * @param data string containing monitor data to be printed
 * @param mon_data pointer to pqos_mon_data structure
 * @param sz_data size of table to fill data into
 * @param llc LLC occupancy data
 * @param mbr remote memory bandwidth data
 * @param mbl local memory bandwidth data
 */
static void
print_text_row(FILE *fp, char *data,
               struct pqos_mon_data *mon_data,
               const size_t sz_data,
               const double llc,
               const double mbr,
               const double mbl)
{
        ASSERT(fp != NULL);
        ASSERT(data != NULL);
        ASSERT(mon_data != NULL);

        fillin_text_row(data, sz_data,
                        mon_data->event,
                        llc, mbr, mbl);

        if (!process_mode()) {
                fprintf(fp, "\n%3u %8.8s %8u%s",
                        mon_data->socket,
                        (char *)mon_data->context,
                        mon_data->rmid,
                        data);
        } else
                fprintf(fp, "\n%6u %6s %8s%s",
                        mon_data->pid, "N/A", "N/A", data);
}

/**
 * @brief Prints row of monitoring data in xml format
 *
 * @param fp pointer to file to direct output
 * @param time pointer to string containing time data
 * @param data string containing monitor data to be printed
 * @param mon_data pointer to pqos_mon_data structure
 * @param sz_data size of table to fill data into
 * @param llc LLC occupancy data
 * @param mbr remote memory bandwidth data
 * @param mbl local memory bandwidth data
 */
static void
print_xml_row(FILE *fp, char *time, char *data,
              struct pqos_mon_data *mon_data,
              const size_t sz_data,
              const double llc,
              const double mbr,
              const double mbl)
{
        ASSERT(fp != NULL);
        ASSERT(time != NULL);
        ASSERT(data != NULL);
        ASSERT(mon_data != NULL);

        fillin_xml_row(data, sz_data,
                       mon_data->event,
                       llc, mbr, mbl);

        if (!process_mode()) {
                fprintf(fp,
                        "%s\n"
                        "\t<time>%s</time>\n"
                        "\t<socket>%u</socket>\n"
                        "\t<core>%s</core>\n"
                        "\t<rmid>%u</rmid>\n"
                        "%s"
                        "%s\n",
                        xml_child_open,
                        time,
                        mon_data->socket,
                        (char *)mon_data->context,
                        mon_data->rmid,
                        data,
                        xml_child_close);
        } else {
                fprintf(fp,
                        "%s\n"
                        "\t<time>%s</time>\n"
                        "\t<pid>%u</pid>\n"
                        "\t<core>%s</core>\n"
                        "\t<rmid>%s</rmid>\n"
                        "%s"
                        "%s\n",
                        xml_child_open,
                        time,
                        mon_data->pid,
                        "N/A",
                        "N/A",
                        data,
                        xml_child_close);
        }
}

/**
 * @brief Prints row of monitoring data in csv format
 *
 * @param fp pointer to file to direct output
 * @param time pointer to string containing time data
 * @param data string containing monitor data to be printed
 * @param mon_data pointer to pqos_mon_data structure
 * @param sz_data size of table to fill data into
 * @param llc LLC occupancy data
 * @param mbr remote memory bandwidth data
 * @param mbl local memory bandwidth data
 */
static void
print_csv_row(FILE *fp, char *time, char *data,
              struct pqos_mon_data *mon_data,
              const size_t sz_data,
              const double llc,
              const double mbr,
              const double mbl)
{
        ASSERT(fp != NULL);
        ASSERT(time != NULL);
        ASSERT(data != NULL);
        ASSERT(mon_data != NULL);

        fillin_csv_row(data, sz_data,
                       mon_data->event,
                       llc, mbr, mbl);

        if (!process_mode()) {
                fprintf(fp,
                        "%s,%u,%s,%u%s\n",
                        time,
                        mon_data->socket,
                        (char *)mon_data->context,
                        mon_data->rmid,
                        data);
        } else {
                fprintf(fp,
                        "%s,%u,%s,%s%s\n",
                        time,
                        mon_data->pid,
                        "N/A",
                        "N/A",
                        data);
        }
}

void monitor_loop(const struct pqos_cap *cap)
{
#define TERM_MIN_NUM_LINES 3

        FILE *fp = fp_monitor;
        const int sel_time = sel_timeout;
        long interval = sel_mon_interval;
        const int top_mode = sel_mon_top_like;
        const char *output_type = sel_output_type;

        double llc_factor = 1, mbr_factor = 1, mbl_factor = 1;
        struct timeval tv_start;
        int ret = PQOS_RETVAL_OK;
        int istty = 0;
        unsigned max_lines = 0;
        const int istext = !strcasecmp(output_type, "text");
        const int isxml = !strcasecmp(output_type, "xml");
        const int iscsv = !strcasecmp(output_type, "csv");

        /* for the dynamic display */
        const size_t sz_header = 128, sz_data = 128;
        char *header = NULL;
        char data[sz_data];
	unsigned mon_number = 0;
	struct pqos_mon_data **mon_data = NULL;

	if (!process_mode())
	        mon_number = (unsigned) sel_monitor_num;
	else
	        mon_number = (unsigned) sel_process_num;

	mon_data = malloc(sizeof(*mon_data) * mon_number);
	if (mon_data == NULL) {
	        printf("Error with memory allocation");
		exit(EXIT_FAILURE);
	}

        if ((!istext)  && (!isxml) && (!iscsv)) {
                printf("Invalid selection of output file type '%s'!\n",
                       output_type);
                free(mon_data);
                return;
        }

        ret = get_event_factors(cap, &llc_factor, &mbr_factor, &mbl_factor);
        if (ret != PQOS_RETVAL_OK) {
                printf("Error in retrieving monitoring scale factors!\n");
                free(mon_data);
                return;
        }

        /**
         * Capture ctrl-c to gracefully stop the loop
         */
        if (signal(SIGINT, monitoring_ctrlc) == SIG_ERR)
                printf("Failed to catch SIGINT!\n");
        if (signal(SIGHUP, monitoring_ctrlc) == SIG_ERR)
                printf("Failed to catch SIGHUP!\n");

        istty = isatty(fileno(fp));

        if (istty) {
                struct winsize w;

                if (ioctl(fileno(fp), TIOCGWINSZ, &w) != -1) {
                        max_lines = w.ws_row;
                        if (max_lines < TERM_MIN_NUM_LINES)
                                max_lines = TERM_MIN_NUM_LINES;
                }
        }

        /**
         * A coefficient to display the data as MB / s
         */
        double coeff = 10.0 / (double)interval;

        /**
         * Interval is passed in  100[ms] units
         * This converts interval to microseconds
         */
        interval = interval * 100000LL;

        gettimeofday(&tv_start, NULL);

        /**
         * Build the header
         */
        if (!isxml) {
                header = (char *) alloca(sz_header);
                if (header == NULL) {
                        printf("Failed to allocate stack frame memory!\n");
                        free(mon_data);
                        return;
                }
                memset(header, 0, sz_header);
                if (istext) {
                        /* Different header for process id's */
                        if (!process_mode())
                                strncpy(header,
                                        "SKT     CORE     RMID",
                                        sz_header - 1);
                        else
                                strncpy(header,
                                        "PID      CORE     RMID",
                                        sz_header - 1);
                        if (sel_events_max & PQOS_MON_EVENT_L3_OCCUP)
                                strncat(header, "    LLC[KB]",
                                        sz_header - strlen(header) - 1);
                        if (sel_events_max & PQOS_MON_EVENT_LMEM_BW)
                                strncat(header, "  MBL[MB/s]",
                                        sz_header - strlen(header) - 1);
                        if (sel_events_max & PQOS_MON_EVENT_RMEM_BW)
                                strncat(header, "  MBR[MB/s]",
                                        sz_header - strlen(header) - 1);
                } else {
                        /* CSV output */
                        if (!process_mode())
                                strncpy(header, "Time,Socket,Core,RMID",
                                        sz_header - 1);
                        else
                                strncpy(header, "Time,PID,Core,RMID",
                                        sz_header - 1);
                        if (sel_events_max & PQOS_MON_EVENT_L3_OCCUP)
                                strncat(header, ",LLC[KB]",
                                        sz_header - strlen(header) - 1);
                        if (sel_events_max & PQOS_MON_EVENT_LMEM_BW)
                                strncat(header, ",MBL[MB/s]",
                                        sz_header - strlen(header) - 1);
                        if (sel_events_max & PQOS_MON_EVENT_RMEM_BW)
                                strncat(header, ",MBR[MB/s]",
                                        sz_header - strlen(header) - 1);
                        fprintf(fp, "%s\n", header);
                }
        }

        while (!stop_monitoring_loop) {
		struct timeval tv_s, tv_e;
                struct tm *ptm = NULL;
                unsigned i = 0;
                struct timespec req, rem;
                long usec_start = 0, usec_end = 0, usec_diff = 0;
                char cb_time[64];

                gettimeofday(&tv_s, NULL);
		ret = pqos_mon_poll(m_mon_grps,
				    (unsigned) mon_number);
		if (ret != PQOS_RETVAL_OK) {
		        printf("Failed to poll monitoring data!\n");
			free(mon_data);
			return;
		}

		memcpy(mon_data, m_mon_grps,
		       mon_number * sizeof(m_mon_grps[0]));

                if (istty)
                        fprintf(fp, "\033[2J");   /**< Clear screen */

                ptm = localtime(&tv_s.tv_sec);
                if (ptm != NULL) {
                        /**
                         * Print time
                         */
                        strftime(cb_time, sizeof(cb_time) - 1,
                                 "%Y-%m-%d %H:%M:%S", ptm);

                        if (istty)
                                fprintf(fp, "\033[0;0H"); /**< Move to
                                                             position 0:0 */
                        if (istext)
                                fprintf(fp, "TIME %s\n", cb_time);
                } else {
                        strncpy(cb_time, "error", sizeof(cb_time) - 1);
                }

                if (top_mode) {
		        qsort(mon_data, mon_number, sizeof(mon_data[0]),
			      mon_qsort_llc_cmp_desc);
		} else if (!process_mode()) {
		        qsort(mon_data, mon_number, sizeof(mon_data[0]),
			      mon_qsort_coreid_cmp_asc);
		}

                if (max_lines > 0) {
                        if ((mon_number+TERM_MIN_NUM_LINES-1) > max_lines)
                                mon_number = max_lines - TERM_MIN_NUM_LINES + 1;
                }

                if (istext)
                        fputs(header, fp);

                for (i = 0; i < mon_number; i++) {
                        double llc = ((double)mon_data[i]->values.llc) *
                                llc_factor;
			double mbr =
                                ((double)mon_data[i]->values.mbm_remote_delta) *
                                mbr_factor * coeff;
			double mbl =
                                ((double)mon_data[i]->values.mbm_local_delta) *
                                mbl_factor * coeff;

                        if (istext) {
                                /* Text */
			        print_text_row(fp, data, mon_data[i],
                                               sz_data, llc, mbr, mbl);
                        } else if (isxml) {
                                /* XML */
                                print_xml_row(fp, cb_time, data, mon_data[i],
                                              sz_data, llc, mbr, mbl);
			} else {
                                /* CSV */
                                print_csv_row(fp, cb_time, data, mon_data[i],
                                              sz_data, llc, mbr, mbl);
                        }
                }
                fflush(fp);

                /**
                 * Move to position 0:0
                 */
                if (istty)
                        fputs("\033[0;0", fp);

                gettimeofday(&tv_e, NULL);

                if (stop_monitoring_loop)
                        break;

                /**
                 * Calculate microseconds to the nearest measurement interval
                 */
                usec_start = ((long)tv_s.tv_usec) +
                        ((long)tv_s.tv_sec * 1000000L);
                usec_end = ((long)tv_e.tv_usec) +
                        ((long)tv_e.tv_sec * 1000000L);
                usec_diff = usec_end - usec_start;

                if (usec_diff < interval) {
                        memset(&rem, 0, sizeof(rem));
                        memset(&req, 0, sizeof(req));

                        req.tv_sec = (interval - usec_diff) / 1000000L;
                        req.tv_nsec =
                                ((interval - usec_diff) % 1000000L) * 1000L;
                        if (nanosleep(&req, &rem) == -1) {
                                /**
                                 * nanosleep() interrupted by a signal
                                 */
                                if (stop_monitoring_loop)
                                        break;
                                req = rem;
                                memset(&rem, 0, sizeof(rem));
                                nanosleep(&req, &rem);
                        }
                }

                if (sel_time >= 0) {
                        gettimeofday(&tv_e, NULL);
                        if ((tv_e.tv_sec - tv_start.tv_sec) > sel_time)
                                break;
                }
        }
        if (isxml)
                fprintf(fp, "%s\n", xml_root_close);

        if (istty)
                fputs("\n\n", fp);

	free(mon_data);

}

void monitor_cleanup(void)
{
        int j = 0;

        /**
         * Close file descriptor for monitoring output
         */
        if (fp_monitor != NULL && fp_monitor != stdout)
                fclose(fp_monitor);
        fp_monitor = NULL;

        /**
         * Free allocated memory
         */
        if (sel_output_file != NULL)
                free(sel_output_file);
        sel_output_file = NULL;
        if (sel_output_type != NULL)
                free(sel_output_type);
        sel_output_type = NULL;

        for (j = 0; j < sel_monitor_num; j++)
                free(m_mon_grps[j]);
}