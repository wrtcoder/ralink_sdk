/*
 * Copyright (C) 2000, 2001, 2002 Jeff Dike (jdike@karaya.com)
 * Licensed under the GPL
 */

#include <pty.h>
#include <stdio.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sched.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <asm/unistd.h>
#include <asm/page.h>
#include <sys/types.h>
#include "user_util.h"
#include "kern_util.h"
#include "user.h"
#include "signal_kern.h"
#include "sysdep/ptrace.h"
#include "sysdep/sigcontext.h"
#include "irq_user.h"
#include "ptrace_user.h"
#include "mem_user.h"
#include "init.h"
#include "os.h"
#include "uml-config.h"
#include "choose-mode.h"
#include "mode.h"
#include "tempfile.h"
#include "kern_constants.h"

#ifdef UML_CONFIG_MODE_SKAS
#include "skas.h"
#include "skas_ptrace.h"
#include "registers.h"
#endif

static int ptrace_child(void *arg)
{
	int ret;
	int pid = os_getpid(), ppid = getppid();
	int sc_result;

	change_sig(SIGWINCH, 0);
	if(ptrace(PTRACE_TRACEME, 0, 0, 0) < 0){
		perror("ptrace");
		os_kill_process(pid, 0);
	}
	kill(pid, SIGSTOP);

	/*This syscall will be intercepted by the parent. Don't call more than
	 * once, please.*/
	sc_result = os_getpid();

	if (sc_result == pid)
		ret = 1; /*Nothing modified by the parent, we are running
			   normally.*/
	else if (sc_result == ppid)
		ret = 0; /*Expected in check_ptrace and check_sysemu when they
			   succeed in modifying the stack frame*/
	else
		ret = 2; /*Serious trouble! This could be caused by a bug in
			   host 2.6 SKAS3/2.6 patch before release -V6, together
			   with a bug in the UML code itself.*/
	_exit(ret);
}

static void fatal_perror(char *str)
{
	perror(str);
	exit(1);
}

static void fatal(char *fmt, ...)
{
	va_list list;

	va_start(list, fmt);
	vprintf(fmt, list);
	va_end(list);
	fflush(stdout);

	exit(1);
}

static void non_fatal(char *fmt, ...)
{
	va_list list;

	va_start(list, fmt);
	vprintf(fmt, list);
	va_end(list);
	fflush(stdout);
}

static int start_ptraced_child(void **stack_out)
{
	void *stack;
	unsigned long sp;
	int pid, n, status;

	stack = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if(stack == MAP_FAILED)
		fatal_perror("check_ptrace : mmap failed");
	sp = (unsigned long) stack + PAGE_SIZE - sizeof(void *);
	pid = clone(ptrace_child, (void *) sp, SIGCHLD, NULL);
	if(pid < 0)
		fatal_perror("start_ptraced_child : clone failed");
	CATCH_EINTR(n = waitpid(pid, &status, WUNTRACED));
	if(n < 0)
		fatal_perror("check_ptrace : clone failed");
	if(!WIFSTOPPED(status) || (WSTOPSIG(status) != SIGSTOP))
		fatal("check_ptrace : expected SIGSTOP, got status = %d",
		      status);

	*stack_out = stack;
	return pid;
}

/* When testing for SYSEMU support, if it is one of the broken versions, we
 * must just avoid using sysemu, not panic, but only if SYSEMU features are
 * broken.
 * So only for SYSEMU features we test mustpanic, while normal host features
 * must work anyway!
 */
static int stop_ptraced_child(int pid, void *stack, int exitcode,
			      int mustexit)
{
	int status, n, ret = 0;

	if(ptrace(PTRACE_CONT, pid, 0, 0) < 0)
		fatal_perror("stop_ptraced_child : ptrace failed");
	CATCH_EINTR(n = waitpid(pid, &status, 0));
	if(!WIFEXITED(status) || (WEXITSTATUS(status) != exitcode)) {
		int exit_with = WEXITSTATUS(status);
		if (exit_with == 2)
			non_fatal("check_ptrace : child exited with status 2. "
				  "Serious trouble happening! Try updating "
				  "your host skas patch!\nDisabling SYSEMU "
				  "support.");
		non_fatal("check_ptrace : child exited with exitcode %d, while "
			  "expecting %d; status 0x%x\n", exit_with,
			  exitcode, status);
		if (mustexit)
			exit(1);
		ret = -1;
	}

	if(munmap(stack, PAGE_SIZE) < 0)
		fatal_perror("check_ptrace : munmap failed");
	return ret;
}

/* Changed only during early boot */
int ptrace_faultinfo = 1;
int ptrace_ldt = 1;
int proc_mm = 1;
int skas_needs_stub = 0;

static int __init skas0_cmd_param(char *str, int* add)
{
	ptrace_faultinfo = proc_mm = 0;
	return 0;
}

/* The two __uml_setup would conflict, without this stupid alias. */

static int __init mode_skas0_cmd_param(char *str, int* add)
	__attribute__((alias("skas0_cmd_param")));

__uml_setup("skas0", skas0_cmd_param,
		"skas0\n"
		"    Disables SKAS3 usage, so that SKAS0 is used, unless \n"
	        "    you specify mode=tt.\n\n");

__uml_setup("mode=skas0", mode_skas0_cmd_param,
		"mode=skas0\n"
		"    Disables SKAS3 usage, so that SKAS0 is used, unless you \n"
		"    specify mode=tt. Note that this was recently added - on \n"
		"    older kernels you must use simply \"skas0\".\n\n");

/* Changed only during early boot */
static int force_sysemu_disabled = 0;

static int __init nosysemu_cmd_param(char *str, int* add)
{
	force_sysemu_disabled = 1;
	return 0;
}

__uml_setup("nosysemu", nosysemu_cmd_param,
"nosysemu\n"
"    Turns off syscall emulation patch for ptrace (SYSEMU) on.\n"
"    SYSEMU is a performance-patch introduced by Laurent Vivier. It changes\n"
"    behaviour of ptrace() and helps reducing host context switch rate.\n"
"    To make it working, you need a kernel patch for your host, too.\n"
"    See http://perso.wanadoo.fr/laurent.vivier/UML/ for further \n"
"    information.\n\n");

static void __init check_sysemu(void)
{
	void *stack;
	int pid, n, status, count=0;

	non_fatal("Checking syscall emulation patch for ptrace...");
	sysemu_supported = 0;
	pid = start_ptraced_child(&stack);

	if(ptrace(PTRACE_SYSEMU, pid, 0, 0) < 0)
		goto fail;

	CATCH_EINTR(n = waitpid(pid, &status, WUNTRACED));
	if (n < 0)
		fatal_perror("check_sysemu : wait failed");
	if(!WIFSTOPPED(status) || (WSTOPSIG(status) != SIGTRAP))
		fatal("check_sysemu : expected SIGTRAP, got status = %d",
		      status);

	n = ptrace(PTRACE_POKEUSR, pid, PT_SYSCALL_RET_OFFSET,
		   os_getpid());
	if(n < 0)
		fatal_perror("check_sysemu : failed to modify system call "
			     "return");

	if (stop_ptraced_child(pid, stack, 0, 0) < 0)
		goto fail_stopped;

	sysemu_supported = 1;
	non_fatal("OK\n");
	set_using_sysemu(!force_sysemu_disabled);

	non_fatal("Checking advanced syscall emulation patch for ptrace...");
	pid = start_ptraced_child(&stack);

	if((ptrace(PTRACE_OLDSETOPTIONS, pid, 0,
		   (void *) PTRACE_O_TRACESYSGOOD) < 0))
		fatal_perror("check_ptrace: PTRACE_OLDSETOPTIONS failed");

	while(1){
		count++;
		if(ptrace(PTRACE_SYSEMU_SINGLESTEP, pid, 0, 0) < 0)
			goto fail;
		CATCH_EINTR(n = waitpid(pid, &status, WUNTRACED));
		if(n < 0)
			fatal_perror("check_ptrace : wait failed");

		if(WIFSTOPPED(status) && (WSTOPSIG(status) == (SIGTRAP|0x80))){
			if (!count)
				fatal("check_ptrace : SYSEMU_SINGLESTEP "
				      "doesn't singlestep");
			n = ptrace(PTRACE_POKEUSR, pid, PT_SYSCALL_RET_OFFSET,
				   os_getpid());
			if(n < 0)
				fatal_perror("check_sysemu : failed to modify "
					     "system call return");
			break;
		}
		else if(WIFSTOPPED(status) && (WSTOPSIG(status) == SIGTRAP))
			count++;
		else
			fatal("check_ptrace : expected SIGTRAP or "
			      "(SIGTRAP | 0x80), got status = %d", status);
	}
	if (stop_ptraced_child(pid, stack, 0, 0) < 0)
		goto fail_stopped;

	sysemu_supported = 2;
	non_fatal("OK\n");

	if ( !force_sysemu_disabled )
		set_using_sysemu(sysemu_supported);
	return;

fail:
	stop_ptraced_child(pid, stack, 1, 0);
fail_stopped:
	non_fatal("missing\n");
}

static void __init check_ptrace(void)
{
	void *stack;
	int pid, syscall, n, status;

	non_fatal("Checking that ptrace can change system call numbers...");
	pid = start_ptraced_child(&stack);

	if((ptrace(PTRACE_OLDSETOPTIONS, pid, 0,
		   (void *) PTRACE_O_TRACESYSGOOD) < 0))
		fatal_perror("check_ptrace: PTRACE_OLDSETOPTIONS failed");

	while(1){
		if(ptrace(PTRACE_SYSCALL, pid, 0, 0) < 0)
			fatal_perror("check_ptrace : ptrace failed");

		CATCH_EINTR(n = waitpid(pid, &status, WUNTRACED));
		if(n < 0)
			fatal_perror("check_ptrace : wait failed");

		if(!WIFSTOPPED(status) ||
		   (WSTOPSIG(status) != (SIGTRAP | 0x80)))
			fatal("check_ptrace : expected (SIGTRAP|0x80), "
			       "got status = %d", status);

		syscall = ptrace(PTRACE_PEEKUSR, pid, PT_SYSCALL_NR_OFFSET,
				 0);
		if(syscall == __NR_getpid){
			n = ptrace(PTRACE_POKEUSR, pid, PT_SYSCALL_NR_OFFSET,
				   __NR_getppid);
			if(n < 0)
				fatal_perror("check_ptrace : failed to modify "
					     "system call");
			break;
		}
	}
	stop_ptraced_child(pid, stack, 0, 1);
	non_fatal("OK\n");
	check_sysemu();
}

extern void check_tmpexec(void);

void os_early_checks(void)
{
	check_ptrace();

	/* Need to check this early because mmapping happens before the
	 * kernel is running.
	 */
	check_tmpexec();
}

static int __init noprocmm_cmd_param(char *str, int* add)
{
	proc_mm = 0;
	return 0;
}

__uml_setup("noprocmm", noprocmm_cmd_param,
"noprocmm\n"
"    Turns off usage of /proc/mm, even if host supports it.\n"
"    To support /proc/mm, the host needs to be patched using\n"
"    the current skas3 patch.\n\n");

static int __init noptracefaultinfo_cmd_param(char *str, int* add)
{
	ptrace_faultinfo = 0;
	return 0;
}

__uml_setup("noptracefaultinfo", noptracefaultinfo_cmd_param,
"noptracefaultinfo\n"
"    Turns off usage of PTRACE_FAULTINFO, even if host supports\n"
"    it. To support PTRACE_FAULTINFO, the host needs to be patched\n"
"    using the current skas3 patch.\n\n");

static int __init noptraceldt_cmd_param(char *str, int* add)
{
	ptrace_ldt = 0;
	return 0;
}

__uml_setup("noptraceldt", noptraceldt_cmd_param,
"noptraceldt\n"
"    Turns off usage of PTRACE_LDT, even if host supports it.\n"
"    To support PTRACE_LDT, the host needs to be patched using\n"
"    the current skas3 patch.\n\n");

#ifdef UML_CONFIG_MODE_SKAS
static inline void check_skas3_ptrace_faultinfo(void)
{
	struct ptrace_faultinfo fi;
	void *stack;
	int pid, n;

	non_fatal("  - PTRACE_FAULTINFO...");
	pid = start_ptraced_child(&stack);

	n = ptrace(PTRACE_FAULTINFO, pid, 0, &fi);
	if (n < 0) {
		ptrace_faultinfo = 0;
		if(errno == EIO)
			non_fatal("not found\n");
		else
			perror("not found");
	}
	else {
		if (!ptrace_faultinfo)
			non_fatal("found but disabled on command line\n");
		else
			non_fatal("found\n");
	}

	init_registers(pid);
	stop_ptraced_child(pid, stack, 1, 1);
}

static inline void check_skas3_ptrace_ldt(void)
{
#ifdef PTRACE_LDT
	void *stack;
	int pid, n;
	unsigned char ldtbuf[40];
	struct ptrace_ldt ldt_op = (struct ptrace_ldt) {
		.func = 2, /* read default ldt */
		.ptr = ldtbuf,
		.bytecount = sizeof(ldtbuf)};

	non_fatal("  - PTRACE_LDT...");
	pid = start_ptraced_child(&stack);

	n = ptrace(PTRACE_LDT, pid, 0, (unsigned long) &ldt_op);
	if (n < 0) {
		if(errno == EIO)
			non_fatal("not found\n");
		else {
			perror("not found");
		}
		ptrace_ldt = 0;
	}
	else {
		if(ptrace_ldt)
			non_fatal("found\n");
		else
			non_fatal("found, but use is disabled\n");
	}

	stop_ptraced_child(pid, stack, 1, 1);
#else
	/* PTRACE_LDT might be disabled via cmdline option.
	 * We want to override this, else we might use the stub
	 * without real need
	 */
	ptrace_ldt = 1;
#endif
}

static inline void check_skas3_proc_mm(void)
{
	non_fatal("  - /proc/mm...");
	if (access("/proc/mm", W_OK) < 0) {
		proc_mm = 0;
		perror("not found");
	}
	else {
		if (!proc_mm)
			non_fatal("found but disabled on command line\n");
		else
			non_fatal("found\n");
	}
}

int can_do_skas(void)
{
	non_fatal("Checking for the skas3 patch in the host:\n");

	check_skas3_proc_mm();
	check_skas3_ptrace_faultinfo();
	check_skas3_ptrace_ldt();

	if(!proc_mm || !ptrace_faultinfo || !ptrace_ldt)
		skas_needs_stub = 1;

	return 1;
}
#else
int can_do_skas(void)
{
	return 0;
}
#endif

int __init parse_iomem(char *str, int *add)
{
	struct iomem_region *new;
	struct stat64 buf;
	char *file, *driver;
	int fd, size;

	driver = str;
	file = strchr(str,',');
	if(file == NULL){
		printf("parse_iomem : failed to parse iomem\n");
		goto out;
	}
	*file = '\0';
	file++;
	fd = open(file, O_RDWR, 0);
	if(fd < 0){
		os_print_error(fd, "parse_iomem - Couldn't open io file");
		goto out;
	}

	if(fstat64(fd, &buf) < 0){
		perror("parse_iomem - cannot stat_fd file");
		goto out_close;
	}

	new = malloc(sizeof(*new));
	if(new == NULL){
		perror("Couldn't allocate iomem_region struct");
		goto out_close;
	}

	size = (buf.st_size + UM_KERN_PAGE_SIZE) & ~(UM_KERN_PAGE_SIZE - 1);

	*new = ((struct iomem_region) { .next		= iomem_regions,
					.driver		= driver,
					.fd		= fd,
					.size		= size,
					.phys		= 0,
					.virt		= 0 });
	iomem_regions = new;
	iomem_size += new->size + UM_KERN_PAGE_SIZE;

	return 0;
 out_close:
	close(fd);
 out:
	return 1;
}


/* Changed during early boot */
int pty_output_sigio = 0;
int pty_close_sigio = 0;

/* Used as a flag during SIGIO testing early in boot */
static volatile int got_sigio = 0;

static void __init handler(int sig)
{
	got_sigio = 1;
}

struct openpty_arg {
	int master;
	int slave;
	int err;
};

static void openpty_cb(void *arg)
{
	struct openpty_arg *info = arg;

	info->err = 0;
	if(openpty(&info->master, &info->slave, NULL, NULL, NULL))
		info->err = -errno;
}

static int async_pty(int master, int slave)
{
	int flags;

	flags = fcntl(master, F_GETFL);
	if(flags < 0)
		return -errno;

	if((fcntl(master, F_SETFL, flags | O_NONBLOCK | O_ASYNC) < 0) ||
	   (fcntl(master, F_SETOWN, os_getpid()) < 0))
		return -errno;

	if((fcntl(slave, F_SETFL, flags | O_NONBLOCK) < 0))
		return -errno;

	return(0);
}

static void __init check_one_sigio(void (*proc)(int, int))
{
	struct sigaction old, new;
	struct openpty_arg pty = { .master = -1, .slave = -1 };
	int master, slave, err;

	initial_thread_cb(openpty_cb, &pty);
	if(pty.err){
		printk("openpty failed, errno = %d\n", -pty.err);
		return;
	}

	master = pty.master;
	slave = pty.slave;

	if((master == -1) || (slave == -1)){
		printk("openpty failed to allocate a pty\n");
		return;
	}

	/* Not now, but complain so we now where we failed. */
	err = raw(master);
	if (err < 0)
		panic("check_sigio : __raw failed, errno = %d\n", -err);

	err = async_pty(master, slave);
	if(err < 0)
		panic("tty_fds : sigio_async failed, err = %d\n", -err);

	if(sigaction(SIGIO, NULL, &old) < 0)
		panic("check_sigio : sigaction 1 failed, errno = %d\n", errno);
	new = old;
	new.sa_handler = handler;
	if(sigaction(SIGIO, &new, NULL) < 0)
		panic("check_sigio : sigaction 2 failed, errno = %d\n", errno);

	got_sigio = 0;
	(*proc)(master, slave);

	close(master);
	close(slave);

	if(sigaction(SIGIO, &old, NULL) < 0)
		panic("check_sigio : sigaction 3 failed, errno = %d\n", errno);
}

static void tty_output(int master, int slave)
{
	int n;
	char buf[512];

	printk("Checking that host ptys support output SIGIO...");

	memset(buf, 0, sizeof(buf));

	while(os_write_file(master, buf, sizeof(buf)) > 0) ;
	if(errno != EAGAIN)
		panic("check_sigio : write failed, errno = %d\n", errno);
	while(((n = os_read_file(slave, buf, sizeof(buf))) > 0) && !got_sigio) ;

	if(got_sigio){
		printk("Yes\n");
		pty_output_sigio = 1;
	}
	else if(n == -EAGAIN) printk("No, enabling workaround\n");
	else panic("check_sigio : read failed, err = %d\n", n);
}

static void tty_close(int master, int slave)
{
	printk("Checking that host ptys support SIGIO on close...");

	close(slave);
	if(got_sigio){
		printk("Yes\n");
		pty_close_sigio = 1;
	}
	else printk("No, enabling workaround\n");
}

void __init check_sigio(void)
{
	if((os_access("/dev/ptmx", OS_ACC_R_OK) < 0) &&
	   (os_access("/dev/ptyp0", OS_ACC_R_OK) < 0)){
		printk("No pseudo-terminals available - skipping pty SIGIO "
		       "check\n");
		return;
	}
	check_one_sigio(tty_output);
	check_one_sigio(tty_close);
}

void os_check_bugs(void)
{
	check_ptrace();
	check_sigio();
}

