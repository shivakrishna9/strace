/* Implementation of strace features over the GDB remote protocol.
 *
 * Copyright (c) 2015 Red Hat Inc.
 * Copyright (c) 2015 Josh Stone <cuviper@gmail.com>
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
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define _GNU_SOURCE 1
#include <stdlib.h>
#include <sys/wait.h>

#include "defs.h"
#include "gdbserver.h"
#include "protocol.h"
#include "signals.h"

/* FIXME jistone: export hacks */
struct tcb *pid2tcb(int pid);
struct tcb *alloctcb(int pid);
void droptcb(struct tcb *tcp);
void newoutf(struct tcb *tcp);
void print_signalled(struct tcb *tcp, const int pid, int status);
void print_exited(struct tcb *tcp, const int pid, int status);
void print_stopped(struct tcb *tcp, const siginfo_t *si, const unsigned int sig);
struct tcb *current_tcp;
int strace_child;

char* gdbserver = NULL;
static struct gdb_conn* gdb = NULL;
static bool gdb_extended = false;
static bool gdb_multiprocess = false;
static bool gdb_vcont = false;

static const char * const gdb_signal_names[] = {
#define SET(symbol, constant, name, string) \
        [constant] = name,
#include "signals.def"
#undef SET
};

static int gdb_signal_map[SUPPORTED_PERSONALITIES][GDB_SIGNAL_LAST];

enum gdb_stop {
        gdb_stop_unknown, // O or F or anything else
        gdb_stop_error, // E
        gdb_stop_signal, // S or T
        gdb_stop_exited, // W
        gdb_stop_terminated, // X

        // specific variants of gdb_stop_signal 05
        gdb_stop_trap, // missing or unrecognized stop reason
        gdb_stop_syscall_entry,
        gdb_stop_syscall_return,
};


struct gdb_stop_reply {
        char *reply;
        size_t size;

        enum gdb_stop type;
        int code; // error, signal, exit status, scno
        int pid; // process id, aka kernel tgid
        int tid; // thread id, aka kernel tid
};

static bool
gdb_ok()
{
        size_t size;
        char *reply = gdb_recv(gdb, &size, false);
        bool ok = size == 2 && !strcmp(reply, "OK");
        free(reply);
        return ok;
}

static int
gdb_map_signal(unsigned int gdb_sig) {
        /* strace "SIG_0" vs. gdb "0" -- it's all zero */
        if (gdb_sig == GDB_SIGNAL_0)
                return 0;

        /* real-time signals are "special", not even fully contiguous */
        if (gdb_sig == GDB_SIGNAL_REALTIME_32)
                return 32;
        if (GDB_SIGNAL_REALTIME_33 <= gdb_sig &&
                        gdb_sig <= GDB_SIGNAL_REALTIME_63)
                return gdb_sig - GDB_SIGNAL_REALTIME_33 + 33;
        if (GDB_SIGNAL_REALTIME_64 <= gdb_sig &&
                        gdb_sig <= GDB_SIGNAL_REALTIME_127)
                return gdb_sig - GDB_SIGNAL_REALTIME_64 + 64;

        const char *gdb_signame = gdb_signal_names[gdb_sig];
        if (!gdb_signame)
                return -1;

        /* many of the other signals line up, but not all. */
        if (gdb_sig < nsignals && !strcmp(gdb_signame, signame(gdb_sig)))
                return gdb_sig;

        /* scan the rest for a match */
        unsigned int sig;
        for (sig = 1; sig < nsignals; ++sig) {
                if (sig == gdb_sig)
                        continue;
                if (!strcmp(gdb_signame, signame(sig)))
                        return sig;
        }

        return -1;
}

static void
gdb_signal_map_init()
{
	unsigned int pers, old_pers = current_personality;

	for (pers = 0; pers < SUPPORTED_PERSONALITIES; ++pers) {
		if (current_personality != pers)
			set_personality(pers);

                unsigned int gdb_sig;
                int *map = gdb_signal_map[pers];
                for (gdb_sig = 0; gdb_sig < GDB_SIGNAL_LAST; ++gdb_sig)
                        map[gdb_sig] = gdb_map_signal(gdb_sig);
	}

	if (old_pers != current_personality)
		set_personality(old_pers);
}

static int
gdb_signal_to_target(struct tcb *tcp, unsigned int signal)
{
        unsigned int pers = tcp->currpers;
        if (pers < SUPPORTED_PERSONALITIES && signal < GDB_SIGNAL_LAST)
                return gdb_signal_map[pers][signal];
        return -1;
}

static void
gdb_parse_thread(const char *id, int *pid, int *tid)
{
        if (*id == 'p') {
                // pPID or pPID.TID
                ++id;
                *pid = gdb_decode_hex_str(id);

                // stop messages should always have the TID,
                // but if not, just use the PID.
                char *dot = strchr(id, '.');
                if (!dot) {
                        *tid = *pid;
                } else {
                        *tid = gdb_decode_hex_str(dot + 1);
                }
        } else {
                // just TID, assume same PID
                *tid = gdb_decode_hex_str(id);
                *pid = *tid;
        }
}

static void
gdb_recv_signal(struct gdb_stop_reply *stop)
{
        char *reply = stop->reply;

        stop->code = gdb_decode_hex_n(&reply[1], 2);
        stop->type = (stop->code == GDB_SIGNAL_TRAP ||
                        stop->code == GDB_SIGNAL_0)
                ? gdb_stop_trap : gdb_stop_signal;

        // tokenize the n:r pairs
        char *info = strdupa(reply + 3);
        char *savetok = NULL, *nr;
        for (nr = strtok_r(info, ";", &savetok); nr;
                        nr = strtok_r(NULL, ";", &savetok)) {
                char *n = strtok(nr, ":");
                char *r = strtok(NULL, "");
                if (!n || !r)
                        continue;

                if (!strcmp(n, "thread")) {
                        gdb_parse_thread(r, &stop->pid, &stop->tid);
                }
                else if (!strcmp(n, "syscall_entry")) {
                        if (stop->type == gdb_stop_trap) {
                                stop->type = gdb_stop_syscall_entry;
                                stop->code = gdb_decode_hex_str(r);
                        }
                }
                else if (!strcmp(n, "syscall_return")) {
                        if (stop->type == gdb_stop_trap) {
                                stop->type = gdb_stop_syscall_return;
                                stop->code = gdb_decode_hex_str(r);
                        }
                }
        }

        // TODO guess architecture by the size of reported registers?
}

static void
gdb_recv_exit(struct gdb_stop_reply *stop)
{
        char *reply = stop->reply;

        stop->type = reply[0] == 'W' ?
                gdb_stop_exited : gdb_stop_terminated;
        stop->code = gdb_decode_hex_str(&reply[1]);

        const char *process = strstr(reply, ";process:");
        if (process) {
                stop->pid = gdb_decode_hex_str(process + 9);

                // we don't really know the tid, so just use PID for now
                // XXX should exits enumerate all threads we know of a process?
                stop->tid = stop->pid;
        }
}

static struct gdb_stop_reply
gdb_recv_stop(struct gdb_stop_reply *stop_reply)
{
        struct gdb_stop_reply stop = {
                .reply = NULL,
                .size = 0,

                .type = gdb_stop_unknown,
                .code = -1,
                .pid = -1,
                .tid = -1,
        };
	char *reply = NULL;
	size_t stop_size;


	if (stop_reply)
	    // pop_notification gave us a cached notification
	    stop = *stop_reply;
	else 
	    stop.reply = gdb_recv(gdb, &stop.size, true);

	if  (gdb_has_non_stop(gdb) && !stop_reply) {
	    /* non-stop packet order:
	       client sends: $vCont;c
	       server sends: OK
	       server sends: %Stop:T05syscall_entry (possibly out of order)
	       client sends: $vStopped
	       server possibly sends 0 or more: T05syscall_entry
	       client sends to each: $vStopped
	       server sends: OK
	    */
	     /* Do we have an out of order notification?  (see gdb_recv) */
	     reply = pop_notification(&stop_size);
	     if (reply) {
		  if (debug_flag)
		       printf ("popped %s\n", reply);
		  stop.reply = reply;
		  reply = gdb_recv(gdb, &stop_size, false); /* vContc OK */
	     }
	     else {
		  if (stop.reply[0] == 'T') {
		       reply = gdb_recv(gdb, &stop_size, false); /* vContc OK */
		  }
		  else {
		       while (stop.reply[0] != 'T') 
			    stop.reply = gdb_recv(gdb, &stop.size, true);
		  }
	     }

	     if (stop.reply[0] == 'T') {
		    do {
			size_t this_size;
			gdb_send(gdb,"vStopped",8);
			reply = gdb_recv(gdb, &this_size, true);
			if (strcmp (reply, "OK") == 0)
			    break;
			push_notification(reply, this_size);
		    } while (true);
	    }
	} // gdb_has_non_stop

        // all good packets are at least 3 bytes
        switch (stop.size >= 3 ? stop.reply[0] : 0) {
                case 'E':
                        stop.type = gdb_stop_error;
                        stop.code = gdb_decode_hex_n(stop.reply + 1, 2);
                        break;
                case 'S':
                case 'T':
                        gdb_recv_signal(&stop);
                        break;
                case 'W':
                case 'X':
                        gdb_recv_exit(&stop);
                        break;
                default:
                        stop.type = gdb_stop_unknown;
                        break;
        }

        return stop;
}

void
gdb_init()
{
        gdb_signal_map_init();

        if (gdbserver[0] == '|')
                gdb = gdb_begin_command(gdbserver + 1);
        else if (strchr(gdbserver, ':') && !strchr(gdbserver, '/')) {
                const char *node = strtok(gdbserver, ":");
                const char *service = strtok(NULL, "");
                gdb = gdb_begin_tcp(node, service);
        } else
                gdb = gdb_begin_path(gdbserver);
	
        if (!gdb_start_noack(gdb))
                error_msg("couldn't enable gdb noack mode");

        static const char multi_cmd[] = "qSupported:multiprocess+";
        gdb_send(gdb, multi_cmd, sizeof(multi_cmd) - 1);

        size_t size;
        char *reply = gdb_recv(gdb, &size, false);
        gdb_multiprocess = strstr(reply, "multiprocess+") != NULL;
        if (!gdb_multiprocess)
                error_msg("couldn't enable gdb multiprocess mode");
        free(reply);

        static const char extended_cmd[] = "!";
        gdb_send(gdb, extended_cmd, sizeof(extended_cmd) - 1);
        gdb_extended = gdb_ok();
        if (!gdb_extended)
                error_msg("couldn't enable gdb extended mode");

        static const char vcont_cmd[] = "vCont?";
        gdb_send(gdb, vcont_cmd, sizeof(vcont_cmd) - 1);
        reply = gdb_recv(gdb, &size, false);
        gdb_vcont = strncmp(reply, "vCont", 5) == 0;
        if (!gdb_vcont)
                error_msg("gdb server doesn't support vCont");
        free(reply);
}

static void
gdb_init_syscalls()
{
        static const char syscall_cmd[] = "QCatchSyscalls:1";
        gdb_send(gdb, syscall_cmd, sizeof(syscall_cmd) - 1);
        if (!gdb_ok())
                error_msg("couldn't enable gdb syscall catching");
}

static struct tcb*
gdb_find_thread(int tid, bool current)
{
        if (tid < 0)
                return NULL;

        /* Look up 'tid' in our table. */
        struct tcb *tcp = pid2tcb(tid);
        if (!tcp) {
                tcp = alloctcb(tid);
                tcp->flags |= TCB_ATTACHED | TCB_STARTUP;
                newoutf(tcp);

                if (!current) {
                        char cmd[] = "Hgxxxxxxxx";
                        sprintf(cmd, "Hg%x", tid);
                        gdb_send(gdb, cmd, strlen(cmd));
                        current = gdb_ok();
                        if (!current)
                                error_msg("couldn't set gdb to thread %d", tid);
                }
                if (current)
                        gdb_init_syscalls();
        }
        return tcp;
}

static void
gdb_enumerate_threads()
{
        // qfThreadInfo [qsThreadInfo]...
        // -> m thread
        // -> m thread,thread
        // -> l (finished)

        static const char qfcmd[] = "qfThreadInfo";

        gdb_send(gdb, qfcmd, sizeof(qfcmd) - 1);

        size_t size;
        char *reply = gdb_recv(gdb, &size, false);
        while (reply[0] == 'm') {
                char *thread;
                for (thread = strtok(reply + 1, ","); thread;
                                thread = strtok(NULL, "")) {
                        int pid, tid;
                        gdb_parse_thread(thread, &pid, &tid);

                        struct tcb *tcp = gdb_find_thread(tid, false);
                        if (tcp && !current_tcp)
                                current_tcp = tcp;
                }

                free(reply);

                static const char qscmd[] = "qsThreadInfo";
                gdb_send(gdb, qscmd, sizeof(qscmd) - 1);
                reply = gdb_recv(gdb, &size, false);
        }

        free(reply);
}

void
gdb_finalize_init()
{
        // We enumerate all attached threads to be sure, especially since we
        // get all threads on vAttach, not just the one pid.
        gdb_enumerate_threads();

        // Everything was stopped from startup_child/startup_attach,
        // now continue them all so the next reply will be a stop packet
        if (gdb_vcont) {
                static const char cmd[] = "vCont;c";
                gdb_send(gdb, cmd, sizeof(cmd) - 1);
        } else {
                static const char cmd[] = "c";
                gdb_send(gdb, cmd, sizeof(cmd) - 1);
        }
}

void
gdb_cleanup()
{
        if (gdb)
                gdb_end(gdb);
        gdb = NULL;
}

void
gdb_startup_child(char **argv)
{
        if (!gdb)
                error_msg_and_die("gdb server not connected!");

        if (!gdb_extended)
                error_msg_and_die("gdb server doesn't support starting processes!");

        size_t i;
        size_t size = 4; // vRun
        for (i = 0; argv[i]; ++i) {
                size += 1 + 2 * strlen(argv[i]); // ;hexified-argument
        }

        char *cmd = malloc(size);
        if (!cmd)
                error_msg_and_die("malloc failed!");
        char *cmd_ptr = cmd;
        memcpy(cmd_ptr, "vRun", 4);
        cmd_ptr += 4;
        for (i = 0; argv[i]; ++i) {
                *cmd_ptr++ = ';';
                const char *arg = argv[i];
                while (*arg) {
                        gdb_encode_hex(*arg++, cmd_ptr);
                        cmd_ptr += 2;
                }
        }

        gdb_send(gdb, cmd, size);
        free(cmd);

        struct gdb_stop_reply stop = gdb_recv_stop(NULL);
        if (stop.size == 0)
                error_msg_and_die("gdb server doesn't support vRun!");
        switch (stop.type) {
                case gdb_stop_error:
                        error_msg_and_die("gdb server failed vRun with %.*s",
                                        (int)stop.size, stop.reply);
                case gdb_stop_trap:
                        break;
                default:
                        error_msg_and_die("gdb server expected vRun trap, got: %.*s",
                                        (int)stop.size, stop.reply);
        }

        pid_t tid = stop.tid;
        free(stop.reply);

        strace_child = tid;

	struct tcb *tcp = alloctcb(tid);
        tcp->flags |= TCB_ATTACHED | TCB_STARTUP;
        newoutf(tcp);
        gdb_init_syscalls();

        // TODO normal strace attaches right before exec, so the first syscall
        // seen is the execve with all its arguments.  Need to emulate that here?
        hide_log_until_execve = 0;
}

void
gdb_startup_attach(struct tcb *tcp)
{
        if (!gdb)
                error_msg_and_die("gdb server not connected!");

        if (!gdb_extended)
                error_msg_and_die("gdb server doesn't support attaching processes!");

        char attach_cmd[] = "vAttach;XXXXXXXX";
        struct gdb_stop_reply stop;
	static const char nonstop_cmd[] = "QNonStop:1";

	gdb_send(gdb, nonstop_cmd, sizeof(nonstop_cmd) - 1);
	if (gdb_ok())
	       gdb_set_non_stop(gdb, true);

        sprintf(attach_cmd, "vAttach;%x", tcp->pid);
        gdb_send(gdb, attach_cmd, strlen(attach_cmd));

	do {
		/*
		  non-stop packet order:
		  client sends: vCont;t
		  server sends: OK
		  server sends: Stop:T05swbreak:;
		  client sends: vStopped
		  server sends: OK
		*/
		char cmd[] = "vCont;t:pXXXXXXXX";
		sprintf(cmd, "vCont;t:p%x.-1", tcp->pid);
		if (!gdb_ok()) {
                        stop.type = gdb_stop_unknown;
                        break;
		}
		gdb_send(gdb, cmd, strlen(cmd));
		stop = gdb_recv_stop(NULL);
	} while (0);
	
	if (stop.type == gdb_stop_unknown) {
		static const char nonstop_cmd[] = "QNonStop:0";
		gdb_send(gdb, nonstop_cmd, sizeof(nonstop_cmd) - 1);
		if (gdb_ok())
			gdb_set_non_stop(gdb, false);
		else
			error_msg_and_die("gdb server doesn't support vAttach!");
		gdb_send(gdb, attach_cmd, strlen(attach_cmd));
		stop = gdb_recv_stop(NULL);
		if (stop.size == 0)
			error_msg_and_die("gdb server doesn't support vAttach!");
		switch (stop.type) {
		case gdb_stop_error:
			error_msg_and_die("gdb server failed vAttach with %.*s",
					  (int)stop.size, stop.reply);
		case gdb_stop_trap:
			break;
		case gdb_stop_signal:
			if (stop.code == 0)
				break;
			// fallthrough
		default:
			error_msg_and_die("gdb server expected vAttach trap, got: %.*s",
					  (int)stop.size, stop.reply);
	    }
	  }

        pid_t tid = stop.tid;
        free(stop.reply);

        if (tid != tcp->pid) {
                droptcb(tcp);
                tcp = alloctcb(tid);
        }
        tcp->flags |= TCB_ATTACHED | TCB_STARTUP;
        newoutf(tcp);
        gdb_init_syscalls();

        if (!qflag)
		fprintf(stderr, "Process %u attached in %s mode\n", tcp->pid,
			gdb_has_non_stop (gdb) ? "non-stop" : "all-stop");
}

void
gdb_detach(struct tcb *tcp)
{
        if (gdb_multiprocess) {
                char cmd[] = "D;XXXXXXXX";
                sprintf(cmd, "D;%x", tcp->pid);
                gdb_send(gdb, cmd, strlen(cmd));
        } else {
                static const char cmd[] = "D";
                gdb_send(gdb, cmd, sizeof(cmd) - 1);
        }

        if (!gdb_ok()) {
                // is it still alive?
                char cmd[] = "T;XXXXXXXX";
                sprintf(cmd, "T;%x", tcp->pid);
                gdb_send(gdb, cmd, strlen(cmd));
                if (gdb_ok())
                        error_msg("gdb server failed to detach %d", tcp->pid);
                // otherwise it's dead, or already detached, fine.
        }
}

// Returns true iff the main trace loop has to continue.
// The gdb connection should be ready for a stop reply on entry,
// and we'll leave it the same way if we return true.
bool
gdb_trace()
{
        struct gdb_stop_reply stop;
        int gdb_sig = 0;
        pid_t tid;

        stop = gdb_recv_stop(NULL);
        do {
                if (stop.size == 0)
                        error_msg_and_die("gdb server gave an empty stop reply!?");
                switch (stop.type) {
                        case gdb_stop_unknown:
                                error_msg_and_die("gdb server stop reply unknown: %.*s",
                                                  (int)stop.size, stop.reply);
                        case gdb_stop_error:
                                // vCont error -> no more processes
                                free(stop.reply);
                                return false;
                        default:
                                break;
                }

                tid = -1;
                struct tcb *tcp = NULL;

                if (gdb_multiprocess) {
                        tid = stop.tid;
                        tcp = gdb_find_thread(tid, true);

                        /* Set current output file */
                        current_tcp = tcp;
                } else if (current_tcp) {
                        tcp = current_tcp;
                        tid = tcp->pid;
                }

                if (tid < 0 || tcp == NULL)
                        error_msg_and_die("couldn't read tid from stop reply: %.*s",
                                        (int)stop.size, stop.reply);

                bool exited = false;
                switch (stop.type) {
                        case gdb_stop_exited:
                                print_exited(tcp, tid, W_EXITCODE(stop.code, 0));
                                droptcb(tcp);
                                exited = true;
                                break;

                        case gdb_stop_terminated:
                                print_signalled(tcp, tid, W_EXITCODE(0,
                                                gdb_signal_to_target(tcp, stop.code)));
                                droptcb(tcp);
                                exited = true;
                                break;

                        default:
                                break;
                }

                if (exited && !gdb_multiprocess) {
                        free(stop.reply);
                        return false;
                }

                get_regs(tid);

                // TODO need code equivalent to PTRACE_EVENT_EXEC?

                /* Is this the very first time we see this tracee stopped? */
                if (tcp->flags & TCB_STARTUP) {
                        tcp->flags &= ~TCB_STARTUP;
                        if (get_scno(tcp) == 1)
                                tcp->s_prev_ent = tcp->s_ent;
                }

                // TODO cflag means we need to update tcp->dtime/stime
                // usually through wait rusage, but how can we do it?

                switch (stop.type) {
                        case gdb_stop_unknown:
                        case gdb_stop_error:
                        case gdb_stop_exited:
                        case gdb_stop_terminated:
                                // already handled above
                                break;

                        case gdb_stop_trap:
                                // misc trap, nothing to do...
                                break;

                        case gdb_stop_syscall_entry:
                                // If we thought we were already in a syscall -- missed
                                // a return? -- skipping this report doesn't do much
                                // good.  Might as well force it to be a new entry
                                // regardless to sync up.
                                tcp->flags &= ~TCB_INSYSCALL;
                                tcp->scno = stop.code;
                                trace_syscall(tcp);
                                break;

                        case gdb_stop_syscall_return:
                                // If we missed the entry, recording a return will only
                                // confuse things, so let's just report the good ones.
                                if (exiting(tcp)) {
                                        tcp->scno = stop.code;
                                        trace_syscall(tcp);
                                }
                                break;

                        case gdb_stop_signal:
                                {
                                        siginfo_t *si = NULL;
                                        size_t siginfo_size;
                                        char *siginfo_reply =
                                                gdb_xfer_read(gdb, "siginfo", "", &siginfo_size);
                                        if (siginfo_reply && siginfo_size == sizeof(siginfo_t))
                                                si = (siginfo_t *) siginfo_reply;

                                        // XXX gdbserver returns "native" siginfo of 32/64-bit target
                                        // but strace expects its own format as PTRACE_GETSIGINFO
                                        // would have given it.
                                        // (i.e. need to reverse siginfo_fixup)
                                        // ((i.e. siginfo_from_compat_siginfo))

                                        gdb_sig = stop.code;
                                        print_stopped(tcp, si, gdb_signal_to_target(tcp, gdb_sig));
                                        free(siginfo_reply);
                                }
                                break;
                }

                free(stop.reply);
                stop.reply = pop_notification(&stop.size);
                if (stop.reply) // cached out of order notification?
                        stop = gdb_recv_stop(&stop);
                else
                        break;
        } while (true);

        if (gdb_sig) {
                if (gdb_vcont) {
                        // send the signal to this target and continue everyone else
                        char cmd[] = "vCont;Cxx:xxxxxxxx;c";
                        sprintf(cmd, "vCont;C%02x:%x;c", gdb_sig, tid);
                        gdb_send(gdb, cmd, strlen(cmd));
                } else {
                        // just send the signal
                        char cmd[] = "Cxx";
                        sprintf(cmd, "C%02x", gdb_sig);
                        gdb_send(gdb, cmd, strlen(cmd));
                }
        } else {
                // just continue everyone
                if (gdb_vcont) {
                        static const char cmd[] = "vCont;c";
                        gdb_send(gdb, cmd, sizeof(cmd) - 1);
                } else {
                        static const char cmd[] = "c";
                        gdb_send(gdb, cmd, sizeof(cmd) - 1);
                }
        }
        return true;
}

char *
gdb_get_regs(pid_t tid, size_t *size)
{
        if (!gdb)
                return NULL;

        /* NB: this assumes gdbserver's current thread is also tid.  If that
         * may not be the case, we should send "HgTID" first, and restore.  */
        gdb_send(gdb, "g", 1);
        return gdb_recv(gdb, size, false);
}

int
gdb_read_mem(pid_t tid, long addr, unsigned int len, bool check_nil, char *out)
{
        if (!gdb) {
                errno = EINVAL;
                return -1;
        }

        /* NB: this assumes gdbserver's current thread is also tid.  If that
         * may not be the case, we should send "HgTID" first, and restore.  */
        while (len) {
                char cmd[] = "mxxxxxxxxxxxxxxxx,xxxx";
                unsigned int chunk_len = len < 0x1000 ? len : 0x1000;
                sprintf(cmd, "m%lx,%x", addr, chunk_len);
                gdb_send(gdb, cmd, strlen(cmd));

                size_t size;
                char *reply = gdb_recv(gdb, &size, false);
                if (size < 2 || reply[0] == 'E' || size > len * 2
                    || gdb_decode_hex_buf(reply, size, out) < 0) {
                        errno = EINVAL;
                        return -1;
                }

                chunk_len = size / 2;
                if (check_nil && strnlen(out, chunk_len) < chunk_len)
                        return 1;

                addr += chunk_len;
                out += chunk_len;
                len -= chunk_len;
        }

        return 0;
}

int
gdb_getfdpath(pid_t tid, int fd, char *buf, unsigned bufsize)
{
        if (!gdb || fd < 0)
                return -1;

        /*
         * As long as we assume a Linux target, we can peek at their procfs
         * just like normal getfdpath does.  Maybe that won't always be true.
         */
        char linkpath[sizeof("/proc/%u/fd/%u") + 2 * sizeof(int)*3];
        sprintf(linkpath, "/proc/%u/fd/%u", tid, fd);
        return gdb_readlink(gdb, linkpath, buf, bufsize);
}
