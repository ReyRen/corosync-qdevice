/*
 * Copyright (c) 2015-2018 Red Hat, Inc.
 *
 * All rights reserved.
 *
 * Author: Jan Friesse (jfriesse@redhat.com)
 *
 * This software licensed under BSD license, the text of which follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of the Red Hat, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>

#include "process-list.h"

/*
 * 1 min timeout
 */
#define WAIT_FOR_NO_RUNNING_REPEATS		6000
#define WAIT_FOR_NO_RUNNING_TIMEOUT		60000
/*
 * 1 sec timeout
 */
/*
#define WAIT_FOR_NO_RUNNING_REPEATS		100
#define WAIT_FOR_NO_RUNNING_TIMEOUT		1000
*/

static int no_executed;
static int no_finished;
static volatile sig_atomic_t sigusr1_received;
static volatile sig_atomic_t sigusr2_received;

static void
signal_usr1_handler(int sig)
{

	sigusr1_received = 1;
}

static void
signal_usr2_handler(int sig)
{

	sigusr2_received = 1;
}

static void
signal_handlers_register(void)
{
	struct sigaction act;

	act.sa_handler = SIG_DFL;
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_RESTART;

	sigaction(SIGCHLD, &act, NULL);

	act.sa_handler = SIG_IGN;
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_RESTART;

	sigaction(SIGPIPE, &act, NULL);

	act.sa_handler = signal_usr1_handler;
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_RESTART;

	sigaction(SIGUSR1, &act, NULL);

	act.sa_handler = signal_usr2_handler;
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_RESTART;

	sigaction(SIGUSR2, &act, NULL);
}

static void
plist_notify(enum process_list_notify_reason reason, const struct process_list_entry *entry,
    void *user_data)
{

	assert(user_data == (void *)0x42);

	switch (reason) {
	case PROCESS_LIST_NOTIFY_REASON_EXECUTED:
		no_executed++;
		break;
	case PROCESS_LIST_NOTIFY_REASON_FINISHED:
		no_finished++;
		break;
	}
}

static char *
find_exec_path(const char *exec)
{
	struct stat stat_buf;
	char *res_path;
	int res;

	assert((res_path = malloc(PATH_MAX)) != NULL);
	memset(res_path, 0, PATH_MAX);

	res = snprintf(res_path, PATH_MAX, "/bin/%s", exec);
	assert(res > 0 && res < PATH_MAX);
	if (stat(res_path, &stat_buf) == 0 && (stat_buf.st_mode & S_IXUSR)) {
		return (res_path);
	}

	res = snprintf(res_path, PATH_MAX, "/usr/bin/%s", exec);
	assert(res > 0 && res < PATH_MAX);
	if (stat(res_path, &stat_buf) == 0 && (stat_buf.st_mode & S_IXUSR)) {
		return (res_path);
	}

	return (NULL);
}

static int
wait_for_no_running(struct process_list *plist, int no_running, int no_in_kill_list)
{
	int timeout;
	int no_repeats;
	int i;

	no_repeats = WAIT_FOR_NO_RUNNING_REPEATS;
	timeout = WAIT_FOR_NO_RUNNING_TIMEOUT / no_repeats;

	for (i = 0; i < no_repeats; i++) {
		assert(process_list_waitpid(plist) == 0);
		if (process_list_get_no_running(plist) == no_running &&
		    process_list_get_kill_list_items(plist) == no_in_kill_list) {
			return (0);
		}

		poll(NULL, 0, timeout);
	}

	return (-1);
}

static int
wait_for_sigusrs_received(void)
{
	int timeout;
	int no_repeats;
	int i;

	no_repeats = WAIT_FOR_NO_RUNNING_REPEATS;
	timeout = WAIT_FOR_NO_RUNNING_TIMEOUT / no_repeats;

	for (i = 0; i < no_repeats; i++) {
		if (sigusr1_received && sigusr2_received) {
			return (0);
		}

		poll(NULL, 0, timeout);
	}

	return (-1);
}


int
main(void)
{
	struct process_list plist;
	struct process_list_entry *plist_entry;
	char *true_path, *false_path;
	char ignore_sigint_cmd[PATH_MAX];
	char ignore_sigintterm_cmd[PATH_MAX];

	assert(snprintf(ignore_sigint_cmd, PATH_MAX,
	    "bash -c \"trap 'echo trap' SIGINT;kill -USR1 %ld;while true;do sleep 1;done\"",
	    (long int)getpid()) < PATH_MAX);

	assert(snprintf(ignore_sigintterm_cmd, PATH_MAX,
	    "bash -c \"trap 'echo trap' SIGINT SIGTERM;kill -USR2 %ld;while true;do sleep 1;done\"",
	    (long int)getpid()) < PATH_MAX);

	assert((true_path = find_exec_path("true")) != NULL);
	assert((false_path = find_exec_path("false")) != NULL);

	signal_handlers_register();

	process_list_init(&plist, 10, 1, plist_notify, (void *)0x42);
	plist_entry = process_list_add(&plist, "test name", "command");
	assert(plist_entry != NULL);
	assert(strcmp(plist_entry->name, "test name") == 0);
	assert(plist_entry->state == PROCESS_LIST_ENTRY_STATE_INITIALIZED);
	assert(plist_entry->exec_argc == 1);
	assert(plist_entry->exec_argv[0] != NULL && strcmp(plist_entry->exec_argv[0], "command") == 0);
	assert(plist_entry->exec_argv[1] == NULL);

	plist_entry = process_list_add(&plist, "test name", "/bin/ping -c \"host wit\\\"h  space\"   notaspace");
	assert(plist_entry != NULL);
	assert(strcmp(plist_entry->name, "test name") == 0);
	assert(plist_entry->state == PROCESS_LIST_ENTRY_STATE_INITIALIZED);
	assert(plist_entry->exec_argc == 4);
	assert(plist_entry->exec_argv[0] != NULL && strcmp(plist_entry->exec_argv[0], "/bin/ping") == 0);
	assert(plist_entry->exec_argv[1] != NULL && strcmp(plist_entry->exec_argv[1], "-c") == 0);
	assert(plist_entry->exec_argv[2] != NULL && strcmp(plist_entry->exec_argv[2], "host wit\"h  space") == 0);
	assert(plist_entry->exec_argv[3] != NULL && strcmp(plist_entry->exec_argv[3], "notaspace") == 0);
	assert(plist_entry->exec_argv[4] == NULL);

	process_list_free(&plist);

	/*
	 * Test no process
	 */
	no_executed = 0;
	no_finished = 0;
	assert(process_list_exec_initialized(&plist) == 0);
	assert(no_executed == 0);
	assert(process_list_get_no_running(&plist) == 0);

	assert(wait_for_no_running(&plist, 0, 0) == 0);

	assert(no_finished == 0);
	assert(process_list_get_summary_result(&plist) == 0);
	assert(process_list_get_summary_result_short(&plist) == 0);

	process_list_free(&plist);

	/*
	 * Test two processes. /bin/true and /bin/false. Accumulated result should be fail
	 */
	plist_entry = process_list_add(&plist, "true", true_path);
	assert(plist_entry != NULL);

	plist_entry = process_list_add(&plist, "false", false_path);
	assert(plist_entry != NULL);

	no_executed = 0;
	no_finished = 0;
	assert(process_list_exec_initialized(&plist) == 0);
	assert(no_executed == 2);
	assert(process_list_get_no_running(&plist) == 2);

	/*
	 * Wait to exit
	 */
	assert(wait_for_no_running(&plist, 0, 0) == 0);

	assert(process_list_waitpid(&plist) == 0);
	assert(process_list_get_no_running(&plist) == 0);
	assert(no_finished == 2);
	assert(process_list_get_summary_result(&plist) == 1);
	assert(process_list_get_summary_result_short(&plist) == 1);

	process_list_free(&plist);

	/*
	 * Test two processes. /bin/true and one non-existing. Accumulated result should be fail
	 */
	plist_entry = process_list_add(&plist, "true", true_path);
	assert(plist_entry != NULL);

	plist_entry = process_list_add(&plist, "false", "/nonexistingdir/nonexistingfile");
	assert(plist_entry != NULL);

	no_executed = 0;
	no_finished = 0;
	assert(process_list_exec_initialized(&plist) == 0);
	assert(no_executed == 2);
	assert(process_list_get_no_running(&plist) == 2);

	/*
	 * Wait to exit
	 */
	assert(wait_for_no_running(&plist, 0, 0) == 0);

	assert(no_finished == 2);
	assert(process_list_get_summary_result(&plist) == 1);
	assert(process_list_get_summary_result_short(&plist) == 1);

	process_list_free(&plist);

	/*
	 * Test three processes /bin/true. Accumulated result should be success.
	 */
	plist_entry = process_list_add(&plist, "true", true_path);
	assert(plist_entry != NULL);

	plist_entry = process_list_add(&plist, "true2", true_path);
	assert(plist_entry != NULL);

	plist_entry = process_list_add(&plist, "true3", true_path);
	assert(plist_entry != NULL);

	no_executed = 0;
	no_finished = 0;
	assert(process_list_exec_initialized(&plist) == 0);
	assert(no_executed == 3);
	assert(process_list_get_no_running(&plist) == 3);

	/*
	 * Wait to exit
	 */
	assert(wait_for_no_running(&plist, 0, 0) == 0);

	assert(no_finished == 3);
	assert(process_list_get_summary_result(&plist) == 0);
	assert(process_list_get_summary_result_short(&plist) == 0);

	process_list_free(&plist);

	/*
	 * Test two processes. /bin/true and cat. Cat blocks so test kill list
	 */
	plist_entry = process_list_add(&plist, "true", true_path);
	assert(plist_entry != NULL);

	plist_entry = process_list_add(&plist, "cat", "/bin/cat /dev/zero");
	assert(plist_entry != NULL);

	no_executed = 0;
	no_finished = 0;
	assert(process_list_exec_initialized(&plist) == 0);
	assert(no_executed == 2);
	assert(process_list_get_no_running(&plist) == 2);

	assert(wait_for_no_running(&plist, 1, 0) == 0);

	assert(no_finished == 1);
	assert(process_list_get_summary_result(&plist) == -1);
	assert(process_list_get_summary_result_short(&plist) == -1);

	process_list_move_active_entries_to_kill_list(&plist);
	assert(process_list_process_kill_list(&plist) == 0);
	/*
	 * There should be 0 running and 0 in kill list
	 */
	assert(wait_for_no_running(&plist, 0, 0) == 0);

	assert(process_list_get_kill_list_items(&plist) == 0);

	assert(process_list_process_kill_list(&plist) == 0);

	process_list_free(&plist);

	/*
	 * Test two bash proceses. One ignores INT and second ignores INT and TERM.
	 */
	sigusr1_received = 0;
	plist_entry = process_list_add(&plist, "ignoresig1", ignore_sigint_cmd);
	assert(plist_entry != NULL);

	sigusr2_received = 0;
	plist_entry = process_list_add(&plist, "ignoresig2", ignore_sigintterm_cmd);
	assert(plist_entry != NULL);

	no_executed = 0;
	no_finished = 0;
	assert(process_list_exec_initialized(&plist) == 0);
	assert(no_executed == 2);
	assert(process_list_get_no_running(&plist) == 2);
	assert(wait_for_sigusrs_received() == 0);

	/*
	 * Wait some time. 2 processes should be running
	 */
	poll(NULL, 0, 500);
	assert(process_list_waitpid(&plist) == 0);

	assert(process_list_get_no_running(&plist) == 2);
	assert(no_finished == 0);
	assert(process_list_get_summary_result(&plist) == -1);
	assert(process_list_get_summary_result_short(&plist) == -1);

	process_list_move_active_entries_to_kill_list(&plist);
	assert(wait_for_no_running(&plist, 0, 2) == 0);
	assert(process_list_process_kill_list(&plist) == 0);
	assert(wait_for_no_running(&plist, 0, 1) == 0);

	assert(process_list_process_kill_list(&plist) == 0);
	assert(wait_for_no_running(&plist, 0, 0) == 0);

	process_list_free(&plist);

	/*
	 * Test 3 processes. Test if entries are properly deallocated
	 */
	process_list_init(&plist, 3, 1, plist_notify, (void *)0x42);
	plist_entry = process_list_add(&plist, "true", true_path);
	assert(plist_entry != NULL);

	plist_entry = process_list_add(&plist, "true2", true_path);
	assert(plist_entry != NULL);

	plist_entry = process_list_add(&plist, "true3", true_path);
	assert(plist_entry != NULL);

	/*
	 * Insert fails
	 */
	plist_entry = process_list_add(&plist, "true4", true_path);
	assert(plist_entry == NULL);

	no_executed = 0;
	no_finished = 0;
	assert(process_list_exec_initialized(&plist) == 0);
	assert(no_executed == 3);
	assert(process_list_get_no_running(&plist) == 3);

	/*
	 * Wait to exit
	 */
	assert(wait_for_no_running(&plist, 0, 0) == 0);

	assert(no_finished == 3);
	assert(process_list_get_summary_result(&plist) == 0);
	assert(process_list_get_summary_result_short(&plist) == 0);

	process_list_move_active_entries_to_kill_list(&plist);

	plist_entry = process_list_add(&plist, "true", true_path);
	assert(plist_entry != NULL);

	sigusr1_received = 0;
	plist_entry = process_list_add(&plist, "ignoresig1", ignore_sigint_cmd);
	assert(plist_entry != NULL);

	sigusr2_received = 0;
	plist_entry = process_list_add(&plist, "ignoresig2", ignore_sigintterm_cmd);
	assert(plist_entry != NULL);

	plist_entry = process_list_add(&plist, "true4", true_path);
	assert(plist_entry == NULL);

	no_executed = 0;
	no_finished = 0;
	assert(process_list_exec_initialized(&plist) == 0);
	assert(no_executed == 3);
	assert(process_list_get_no_running(&plist) == 3);
	assert(wait_for_sigusrs_received() == 0);

	assert(wait_for_no_running(&plist, 2, 0) == 0);

	assert(process_list_get_no_running(&plist) == 2);
	assert(no_finished == 1);
	assert(process_list_get_summary_result(&plist) == -1);
	assert(process_list_get_summary_result_short(&plist) == -1);

	plist_entry = process_list_add(&plist, "true4", true_path);
	assert(plist_entry == NULL);

	process_list_move_active_entries_to_kill_list(&plist);

	plist_entry = process_list_add(&plist, "true4", true_path);
	assert(plist_entry != NULL);

	plist_entry = process_list_add(&plist, "true5", true_path);
	assert(plist_entry == NULL);

	assert(process_list_process_kill_list(&plist) == 0);
	assert(wait_for_no_running(&plist, 0, 1) == 0);

	assert(process_list_process_kill_list(&plist) == 0);
	assert(wait_for_no_running(&plist, 0, 0) == 0);

	process_list_move_active_entries_to_kill_list(&plist);
	assert(process_list_get_summary_result(&plist) == 0);
	assert(process_list_get_summary_result_short(&plist) == 0);

	plist_entry = process_list_add(&plist, "true", true_path);
	assert(plist_entry != NULL);

	plist_entry = process_list_add(&plist, "true2", true_path);
	assert(plist_entry != NULL);

	plist_entry = process_list_add(&plist, "true3", true_path);
	assert(plist_entry != NULL);

	plist_entry = process_list_add(&plist, "true4", true_path);
	assert(plist_entry == NULL);

	process_list_free(&plist);

	/*
	 * Test 3 processes and difference between summary and short-circuit summary
	 */
	process_list_init(&plist, 3, 1, plist_notify, (void *)0x42);
	plist_entry = process_list_add(&plist, "true", true_path);
	assert(plist_entry != NULL);

	plist_entry = process_list_add(&plist, "false", false_path);
	assert(plist_entry != NULL);

	plist_entry = process_list_add(&plist, "loop", "bash -c \"while true;do sleep 1;done\"");
	assert(plist_entry != NULL);

	plist_entry = process_list_add(&plist, "true4", true_path);
	assert(plist_entry == NULL);

	no_executed = 0;
	no_finished = 0;
	assert(process_list_exec_initialized(&plist) == 0);
	assert(no_executed == 3);
	assert(process_list_get_no_running(&plist) == 3);

	/*
	 * Wait to exit
	 */
	assert(wait_for_no_running(&plist, 1, 0) == 0);

	assert(no_finished == 2);
	assert(process_list_get_summary_result(&plist) == -1);
	assert(process_list_get_summary_result_short(&plist) == 1);

	process_list_move_active_entries_to_kill_list(&plist);
	assert(process_list_process_kill_list(&plist) == 0);
	assert(wait_for_no_running(&plist, 0, 0) == 0);

	process_list_free(&plist);

	/*
	 * Test process_list_killall by running two bash proceses.
	 * One ignores INT and second ignores INT and TERM. Waiting for maximum of 2 sec
	 */
	sigusr1_received = 0;
	plist_entry = process_list_add(&plist, "ignoresig1", ignore_sigint_cmd);
	assert(plist_entry != NULL);

	sigusr2_received = 0;
	plist_entry = process_list_add(&plist, "ignoresig2", ignore_sigintterm_cmd);
	assert(plist_entry != NULL);

	no_executed = 0;
	no_finished = 0;
	assert(process_list_exec_initialized(&plist) == 0);
	assert(no_executed == 2);
	assert(process_list_get_no_running(&plist) == 2);
	assert(wait_for_sigusrs_received() == 0);

	/*
	 * Ensure processes are running after pause
	 */
	poll(NULL, 0, 500);
	assert(process_list_waitpid(&plist) == 0);

	assert(process_list_get_no_running(&plist) == 2);
	assert(no_finished == 0);
	assert(process_list_get_summary_result(&plist) == -1);
	assert(process_list_get_summary_result_short(&plist) == -1);

	assert(process_list_killall(&plist, 2000) == 0);
	assert(process_list_get_kill_list_items(&plist) == 0);

	process_list_free(&plist);

	/*
	 * Empty killall exits with sucess result
	 */
	assert(process_list_killall(&plist, 2000) == 0);

	process_list_free(&plist);

	return (0);
}
