/*
 * mock-helper.c: help mock perform tasks needing root privileges
 */

#define _GNU_SOURCE

#include "config.h"

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <grp.h>
#include <libgen.h>
#include <limits.h>
#include <fcntl.h>
#include <ctype.h>
#include <signal.h>
#include <dirent.h>

#ifdef USE_SELINUX
#include <selinux/selinux.h>
#endif

/* pull in configure'd defines */
static char *rootsdir = ROOTSDIR;

static char const * const ALLOWED_ENV[] =
{
  "dist",
  "ftp_proxy", "http_proxy", "https_proxy", "no_proxy", "PS1",
};

#define ALLOWED_ENV_SIZE (sizeof (ALLOWED_ENV) / sizeof (ALLOWED_ENV[0]))

/*
 * helper functions
 */

void
usage ()
{
  printf ("Usage: mock-helper [command]\n");
  exit (1);
}

/* print formatted string to stderr, print newline and terminate */
void
error (const char *format, ...)
{
  va_list ap;

  va_start (ap, format);
  fprintf (stderr, "mock-helper: error: ");
  vfprintf (stderr, format, ap);
  va_end (ap);
  fprintf (stderr, "\n");
  exit (1);
}

/* print formatted string to stderr, print newline and continue */
void
warning (const char *format, ...)
{
  va_list ap;

  va_start (ap, format);
  fprintf (stderr, "mock-helper: warning: ");
  vfprintf (stderr, format, ap);
  va_end (ap);
  fprintf (stderr, "\n");
}

/*
 * perform checks on the given filesystem entity
 * - is the given entry under the allowed hierarchy ?
 * - are we not being tricked by using .. ?
 */
void
check_allowed (const char *given)
{
  char last;

  /* does given start with rootsdir ? */
  if (strncmp (given, rootsdir, strlen (rootsdir)) != 0)
    error ("%s: not under allowed directory", given);

  /* does it try to fool us by using .. ? */
  if (strstr (given, "..") != 0)
    error ("%s: contains '..'", given);

  /* does it try to fool us into following symlinks by having a trailing / ? */
  last = rootsdir[strlen (given) - 1];
  if (last == '/')
    error ("%s: ends with '/'", given);
}


/*
 * perform checks on the given dir
 * - is the given dir under the allowed hierarchy ?
 * - is it an actual dir ?
 * - are we not being tricked by using . or .. ?
 */
void
check_dir_allowed (const char *given)
{
  struct stat buf;
  int retval;

  /* basic checks */
  check_allowed(given);

  /* are we chrooting to an actual directory (not a symlink or anything) ? */
  retval = lstat (given, &buf);
  if (retval != 0)
    error ("%s: %s", given, strerror (errno));

  //printf ("DEBUG: mode: %o\n", buf.st_mode);
  if (S_ISLNK (buf.st_mode))
    error ("%s: symbolic link", given);
  if (!(S_ISDIR (buf.st_mode)))
    error ("%s: not a directory", given);
}

/*
 * perform checks on the given file
 * - is the given file under the allowed hierarchy ?
 * - is it an actual file ?
 * - are we not being tricked by using .. ?
 */
void
check_file_allowed (const char *given)
{
  struct stat buf;
  int retval;

  /* basic checks */
  check_allowed(given);

  /* are we working with an actual file ? */
  retval = lstat (given, &buf);
  if (retval != 0)
    error ("%s: %s", given, strerror (errno));

  //printf ("DEBUG: mode: %o\n", buf.st_mode);
  if (S_ISLNK (buf.st_mode))
    error ("%s: symbolic link", given);
  if (!(S_ISREG (buf.st_mode)))
    error ("%s: not a regular file", given);
}


/* argv[0] should by convention be the binary name to be executed */
void
do_command (const char *filename, char *const argv[], int use_selinux_preload)
{
  /* do not trust user environment;
   * copy over allowed env vars, after setting PATH and HOME ourselves
   */
  char *env[3 + ALLOWED_ENV_SIZE] = {
    [0] = "PATH=/bin:/usr/bin:/usr/sbin",
    [1] = "HOME=/root"
  };
  int retval;
  //char **arg;
  size_t idx=2;
  size_t i;
#ifdef USE_SELINUX
  char *ld_preload;
#endif

  /* elevate privileges */
  setreuid (geteuid (), geteuid ());
  //printf ("DEBUG: First argument: %s\n", *argv);
  //printf ("DEBUG: Executing %s\n", filename);
  /* FIXME: for a debug option */

  //printf ("Executing %s ", filename);
  //for (arg = (char **) &(argv[1]); *arg; ++arg)
  //   printf ("%s ", *arg);
  //printf ("\n");


#ifdef USE_SELINUX
  /* add LD_PRELOAD for our selinux lib if selinux is in use is set */
  if ((is_selinux_enabled() > 0) && (use_selinux_preload == 1))
  {
    ld_preload = strdup("LD_PRELOAD=libselinux-mock.so");
    printf("adding ld_preload of %s\n", ld_preload);
    env[idx++] = ld_preload;
  }
#endif

  for (i = 0; i < ALLOWED_ENV_SIZE; ++i)
  {
    char *ptr = getenv (ALLOWED_ENV[i]);
    if (ptr==0) continue;
    ptr -= strlen (ALLOWED_ENV[i]) + 1;
    env[idx++] = ptr;
  }

  retval = execve (filename, argv, env);
  error ("executing %s: %s", filename, strerror (errno));
}

/*
 * actual command implementations
 */


void
do_chroot (int argc, char *argv[])
{
  if (argc < 3)
    error ("No directory given for chroot !");
  //printf ("DEBUG: rootsdir: %s\n", rootsdir);

  /* do we allow this dir ? */
  check_dir_allowed (argv[2]);
 
  do_command ("/usr/sbin/chroot", &(argv[1]), 0);
}

/*
 * allow proc mounts:
 * mount -t proc proc (root)/proc
 * allow devpts mounts:
 * mount -t devpts devpts (root)/dev/pts
 */
void
do_mount (int argc, char *argv[])
{
  /* see if we have enough arguments for it to be what we want, ie. 5 */
  if (argc < 5)
    error ("not enough arguments");

  /* see if it's -t proc or -t devpts */
  if ((strncmp ("-t", argv[2], 2) == 0) &&
           (strncmp ("proc", argv[3], 4) == 0)) {
    /* see if we're mounting proc to somewhere in rootsdir */
    if (strncmp (rootsdir, argv[5], strlen (rootsdir)) != 0)
      error ("proc: mount not allowed on %s", argv[5]);
  }
  else if ((strncmp ("-t", argv[2], 2) == 0) &&
           (strncmp ("devpts", argv[3], 6) == 0)) {
    if (argc < 5)
      error ("devpts: not enough mount arguments");
    /* see if we're mounting devpts to somewhere in rootsdir */
    else if (strncmp (rootsdir, argv[5], strlen (rootsdir)) != 0)
      error ("devpts: mount not allowed on %s", argv[5]);
  }
  else if ((strncmp("--bind", argv[2], 6) == 0) &&
		   (strncmp("/dev", argv[3], 4) == 0)) {
	  /* make sure we're only mounting /dev in the chroot */
	  if (strncmp(rootsdir, argv[4], strlen(rootsdir)) != 0)
		  error("can only bindmount /dev in chroot");
  }
  else
    error ("unallowed mount type");

  /* all checks passed, execute */
  do_command ("/bin/mount", &(argv[1]), 0);
}

/* clean out a chroot dir */
void
do_rm (int argc, char *argv[])
{
  /* enough arguments ? mock-helper rm -rf (rootdir), 4 */
  if (argc < 4)
    error ("not enough arguments");

  /* see if we're doing rm -rf */
  if (strncmp ("-rf", argv[2], 4) != 0)
    error ("%s: options not allowed", argv[2]);

  /* see if we're doing -rf on a dir under rootsdir */
  check_dir_allowed (argv[3]);

  /* all checks passed, execute */
  do_command ("/bin/rm", &(argv[1]), 0);
}

/* perform rpm commands on root */
void
do_rpm (int argc, char *argv[])
{
  /* enough arguments ? mock-helper rpm --root (rootdir) ... , 4 */
  if (argc < 4)
    error ("not enough arguments");

  /* --root */
  if (strncmp ("--root", argv[2], 6) != 0)
    error ("%s: options not allowed", argv[2]);

  /* check given dir */
  check_dir_allowed (argv[3]);

  /* all checks passed, execute */
  do_command ("/bin/rpm", &(argv[1]), 0);
}


void
do_yum (int argc, char *argv[])
{
  /* enough arguments ? mock-helper yum --installroot (rootdir) ... , 4 */
  if (argc < 4)
    error ("not enough arguments");

  /* --root */
  if (strncmp ("--installroot", argv[2], 6) != 0)
    error ("%s: options not allowed", argv[2]);

  /* check given dir */
  check_dir_allowed (argv[3]);

  /* all checks passed, execute */
  do_command ("/usr/libexec/mock-yum", &(argv[1]), 1);
}


/* unmount archivesdir and proc */
void
do_umount (int argc, char *argv[])
{
  /* enough arguments ? mock-helper umount (dir), 3 */
  if (argc < 3)
    error ("not enough arguments");

  /* see if we're unmounting from somewhere in rootsdir */
  check_dir_allowed (argv[2]);

  /* all checks passed, execute */
  do_command ("/bin/umount", &(argv[1]), 1);
}

/* make /dev/ device nodes */
void
do_mknod (int argc, char *argv[])
{
  /* enough arguments ? mock-helper mknod (name) -m (mode) (type) (major) (minor), 8 */
  if (argc < 8)
    error ("not enough arguments");

  /* check given file */
  if (strncmp (argv[2], rootsdir, strlen (rootsdir)) != 0)
    error ("%s: not under allowed directory (%s)", argv[2], rootsdir);

  /* does it try to fool us by using .. ? */
  if (strstr (argv[2], "..") != 0)
    error ("%s: contains '..'", argv[2]);

  /* does it have a trailing / ? */
  int last = argv[2][strlen (argv[2]) - 1];
  if (last == '/')
    error ("%s: ends with '/'", argv[2]);

  /* -m */
  if (strncmp ("-m", argv[3], 2) != 0)
    error ("%s: options not allowed", argv[3]);

  /* removed specific checks so we can make more than just /dev/null */
  /* all checks passed, execute */
  do_command ("/bin/mknod", &(argv[1]), 0);
}

void
do_unpack(int argc, char *argv[])
{
  char *new_argv[5];

  if (argc < 4)
    error ("not enough arguments");
  
  check_dir_allowed (argv[2]);

  if (chdir(argv[2]) != 0)
    error ("could not change dir");

  new_argv[0] = "tar";
  new_argv[1] = "--same-owner";

  /* select compression */
  if (strstr(argv[3], ".bz2"))
    new_argv[2] = "-jxpf";
  else if (strstr(argv[3], ".gz"))
    new_argv[2] = "-zxpf";
  else
    new_argv[2] = "-xpf";

  new_argv[3] = argv[3];
  new_argv[4] = NULL;
  
  do_command("/bin/tar", new_argv, 0);
}


void
do_pack(int argc, char *argv[])
{
  char *new_argv[6];
  char *cache_dir = 0;
  char *argv_copy = 0;
  struct group *gr = 0;

  if (argc < 5)
    error ("not enough arguments");
  
  check_dir_allowed (argv[2]);

  if (chdir(argv[2]) != 0)
    error ("could not change dir");

  /* create root-cache dir with appropriate perms */
  gr = getgrnam("mock");
  argv_copy = strdup(argv[3]);
  cache_dir = dirname(argv_copy);

  check_dir_allowed (cache_dir);

  mkdir(cache_dir, 0750);
  if (gr)
    chown(cache_dir, 0, gr->gr_gid);
  free(argv_copy);
  argv_copy = 0;

  new_argv[0] = "tar";
  new_argv[1] = "--one-file-system";

  /* select compression */
  if (strstr(argv[3], ".bz2"))
    new_argv[2] = "-jcf";
  else if (strstr(argv[3], ".gz"))
    new_argv[2] = "-zcf";
  else
    new_argv[2] = "-cf";

  new_argv[3] = argv[3];
  new_argv[4] = argv[4];
  new_argv[5] = NULL;
  
  do_command("/bin/tar", new_argv, 0);
}

void
do_chown (int argc, char *argv[])
{
	int i;

	/* validate argument vector length */
	if (argc < 4)
		error("do_chown: not enough arguments (%d)\n", argc);
	
	/* verify files are legal */
	for (i = 3; i < argc; i++)
		check_allowed(argv[i]);

	do_command("/bin/chown", &(argv[1]), 1);
}

void
do_chmod (int argc, char *argv[])
{
	int i;

	/* validate argument vector length */
	if (argc < 4)
		error("do_chmod: not enough arguments (%d)\n", argc);
	
	/* verify files are legal */
	for (i = 3; i < argc; i++)
		check_allowed(argv[i]);

	do_command("/bin/chmod", &(argv[1]), 1);
}

const char *read_cmdline (pid_t pid)
{
  char cmdline_fname[32];
  static char cmdline[LINE_MAX];
  int fd;
  ssize_t got;
  char *s;

  if (snprintf (cmdline_fname, sizeof (cmdline_fname), "/proc/%d/cmdline",
      (int) pid) < 0)
    return NULL;
  fd = open (cmdline_fname, O_RDONLY);
  if (fd == -1) {
    warning ("open (\"%s\"): %s", cmdline_fname, strerror (errno));
    return NULL;
  }
  got = read (fd, cmdline, sizeof (cmdline) - 1);
  if (got == -1)
    warning ("read (\"%s\"): %s", cmdline_fname, strerror (errno));
  if (close (fd))
    warning ("close (\"%s\"): %s", cmdline_fname, strerror (errno));
  if (got < 0)
    return NULL;
  /* Convert '\0' argument delimiters to spaces.  */
  for (s = cmdline; s < cmdline + got; s++)
    if (*s == 0)
      *s = ' ';
  /* Trim the trailing spaces (typically single '\0'->' ').  */
  while (s > cmdline && isspace (s[-1]) != 0)
    s--;
  *s = 0;
  return cmdline;
}

void orphanskill_pid (pid_t pid)
{
  const char *cmdline;

  /* Should not happen.  */
  if (pid == getpid())
    error ("We as PID %d should not be chrooted", (int) pid);

  cmdline = read_cmdline (pid);
  if (cmdline == 0)
    cmdline = "<error>";
  warning ("Killed -9 orphan PID %d: %s", (int) pid, cmdline);
  if (kill (pid, SIGKILL))
  {
    /* It may just be a race.  */
    warning ("kill (%d, SIGKILL): %s", (int) pid, strerror (errno));
    return;
  }
  /* Do not waitpid(2) as it cannot be our direct descendant and it gets
     cleaned up by init(8).  */
}

void
do_orphanskill (int argc, char *argv[])
{
  DIR *dir;
  struct dirent *dirent;
  const char *chrootdir;
  size_t chrootdir_len;
  char *link_buf;

  if (argc < 3)
    error ("No directory given for chroot !");
  //printf ("DEBUG: rootsdir: %s\n", rootsdir);

  chrootdir = argv[2];
  /* do we allow this dir ? */
  check_dir_allowed (chrootdir);
  chrootdir_len = strlen (chrootdir) + 1;
  link_buf = malloc (chrootdir_len);
  if (link_buf == 0)
    error ("malloc (%lu): %s", (unsigned long) chrootdir_len,
	   strerror (errno));
 
  dir = opendir ("/proc");
  if (dir == 0)
    error ("opendir (\"/proc\"): %s", strerror (errno));

  while ((dirent = readdir (dir))) {
    const char *cs;
    char proc_root[64];
    int proc_root_got;
    int pid;
    ssize_t link_buf_got;

	/* reset any errors from the previous iteration */
	errno = 0;

    if (dirent->d_type != DT_DIR)
      continue;

    /* Check /^\d+$/:  */
    for (cs = dirent->d_name; *cs; cs++)
      if (isdigit (*cs) == 0)
        break;

    if (cs == dirent->d_name || *cs != 0)
      continue;

    pid = atoi (dirent->d_name);

    proc_root_got = snprintf (proc_root, sizeof (proc_root), "/proc/%d/root",
			      pid);

    if (proc_root_got <= 0 || proc_root_got >= sizeof (proc_root)) {
		warning("/proc/%d/root: %s", pid, strerror (errno));
		continue;
	}

    link_buf_got = readlink (proc_root, link_buf, chrootdir_len);

    /* Errors may occur due to races.  */
    if (link_buf_got != chrootdir_len - 1
        || memcmp (link_buf, chrootdir, chrootdir_len - 1) != 0)
      continue;

    orphanskill_pid (pid);
  }
}

int
main (int argc, char *argv[])
{

  /* verify input */
  if (argc < 2) usage ();
  
  /* see which command we are trying to run */
  if (strncmp ("chroot", argv[1], 6) == 0)
    do_chroot (argc, argv);
  else if (strncmp ("mount", argv[1], 5) == 0)
    do_mount (argc, argv);
  else if (strncmp ("rm", argv[1], 2) == 0)
    do_rm (argc, argv);
  else if (strncmp ("umount", argv[1], 6) == 0)
    do_umount (argc, argv);
  else if (strncmp ("rpm", argv[1], 3) == 0)
    do_rpm (argc, argv);
  else if (strncmp ("mknod", argv[1], 5) == 0)
    do_mknod (argc, argv);
  else if (strncmp ("yum", argv[1], 3) == 0)
    do_yum (argc, argv);
  else if (strncmp ("unpack", argv[1], 6) == 0)
    do_unpack (argc, argv);
  else if (strncmp ("pack", argv[1], 4) == 0)
    do_pack (argc, argv);
  else if (strncmp ("chown", argv[1], 5) == 0)
	  do_chown(argc, argv);
  else if (strncmp ("chmod", argv[1], 5) == 0)
	  do_chmod(argc, argv);
  else if (strncmp ("orphanskill", argv[1], 11) == 0)
    do_orphanskill (argc, argv);
  else
  {
    error ("Command %s not recognized !\n", argv[1]);
    exit (1);
  }
  exit (0);
}