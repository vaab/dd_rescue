/* dd_rescue.c */
/* 
 * dd_rescue copies your data from one file to another
 * files might as well be block devices, such as hd partitions
 * unlike dd, it does not necessarily abort on errors, but
 * continues to copy the disk, possibly leaving holes behind.
 * Also, it does NOT truncate the output file, so you can copy 
 * more and more pieces of your data, as time goes by.
 * So, this tool is suitable for rescueing data of crashed disk,
 * and that's the reason, it has been written by me.
 *
 * (c) Kurt Garloff <garloff@suse.de>, 11/97, 10/99
 * Copyright: GNU GPL
 */

/*
 * TODO:
 * - Use termcap to fetch cursor up code
 */

#ifndef VERSION
# define VERSION "(unknown)"
#endif

#define ID "$Id$"

#ifndef SOFTBLOCKSIZE
# define SOFTBLOCKSIZE 16384
#endif

#ifndef HARDBLOCKSIZE
# define HARDBLOCKSIZE 512
#endif

#define _GNU_SOURCE
//#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <getopt.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>

int softbs, hardbs;
int maxerr, nrerr, reverse, trunc, abwrerr, sparse;
int verbose, quiet, interact, force;
char* buf;
char *lname, *iname, *oname;
off_t ipos, opos, xfer, lxfer, sxfer, fxfer, maxxfer;

int ides, odes;
FILE *log;
struct timeval starttime, lasttime, currenttime;
struct timezone tz;
clock_t startclock;

char* up = "\x1b[A"; //] 
char* down = "\n";
char* right = "\x1b[C"; //]

inline float difftimetv (struct timeval* t2, struct timeval *t1)
{
  return (float) (t2->tv_sec - t1->tv_sec) + 1e-6 * (float)(t2->tv_usec - t1->tv_usec);
}

int openfile (char* fname, int flags)
{
  int des = open (fname, flags, 0640);
  if (des == -1) {
    char buf[128];
    snprintf (buf, 128, "dd_rescue: (fatal): open \"%s\" failed", fname);
    perror (buf); exit(17);
  }
  return des;
}

void cleanup ()
{
  if (odes != -1) {
    /* Make sure, the output file is expanded to the last (first) position */
    pwrite (odes, buf, 0, opos);
    close (odes); 
  }
  if (ides != -1) close (ides);
  if (log) fclose (log);
  if (buf) free (buf);
}

void doprint (FILE* file, int bs, clock_t cl, float t1, float t2, int sync)
{
  fprintf (file, "dd_rescue: (info): ipos:%12.1fk, opos:%12.1fk, xferd:%12.1fk\n",
	   (float)ipos/1024, (float)opos/1024, (float)xfer/1024);
  fprintf (file, "             %s  %s  errs:%7i, errxfer:%12.1fk, succxfer:%12.1fk\n",
	   (reverse? "-": " "), (bs==hardbs? "*": " "), nrerr, 
	   (float)fxfer/1024, (float)sxfer/1024);
  if (sync || (file != stdin && file != stdout) )
  fprintf (file, "             +curr.rate:%9.0fkB/s, avg.rate:%9.0fkB/s, avg.load:%5.1f\%\n",
	   (float)(xfer-lxfer)/(t2*1024),
	   (float)xfer/(t1*1024),
	   100*(cl-startclock)/(CLOCKS_PER_SEC*t1));
  else
  fprintf (file, "             -curr.rate:%s%s%s%s%s%s%s%s%skB/s, avg.rate:%9.0fkB/s, avg.load:%5.1f\%\n",
	   right, right, right, right, right, right, right, right, right,
	   (float)xfer/(t1*1024),
	   100*(cl-startclock)/(CLOCKS_PER_SEC*t1));

}

void printstatus (FILE* file1, FILE* file2, int bs, int sync)
{
  float t1, t2; 
  clock_t cl;
  if (sync) fsync (odes);
  gettimeofday (&currenttime, NULL);
  t1 = difftimetv (&currenttime, &starttime);
  t2 = difftimetv (&currenttime, &lasttime);
  cl = clock ();

  if (file1 == stderr || file1 == stdout) 
    fprintf (file1, "%s%s%s", up, up, up);
  if (file2 == stderr || file2 == stdout) 
    fprintf (file2, "%s%s%s", up, up, up);

  if (file1) doprint (file1, bs, cl, t1, t2, sync);
  if (file2) doprint (file2, bs, cl, t1, t2, sync);
  if (sync) {
    memcpy (&lasttime, &currenttime, sizeof(lasttime));
    lxfer = xfer;
  }
}

/* Write to file and simultaneously log to logfile, if exsiting */
int fplog (FILE* file, char * fmt, ...)
{
  int ret = 0;
  va_list vl; 
  va_start (vl, fmt);
  if (file) ret = vfprintf (file, fmt, vl);
  va_end (vl);
  if (log) {
    va_start (vl, fmt);
    ret = vfprintf (log, fmt, vl);
    va_end (vl);
  }
  return ret;
};

/* is the block zero ? */
int blockzero (char* blk, int ln)
{
  char* ptr = blk;
  while ((ptr-blk) < ln) if (*(ptr++)) return 0;
  return 1;
}


/* can be invoked in two ways: bs==hardbs or bs==softbs */
int copyfile (off_t max, int bs)
{
  int errs = 0;
  /* expand file to the right length */
  pwrite (odes, buf, 0, opos);
  while ((!max || (max-xfer > 0)) && ((!reverse) || (ipos > 0 && opos > 0))) {
    int err;
    ssize_t rd = 0;
    ssize_t toread = ((max && max-xfer < bs)? (max-xfer): bs);
    if (reverse) {
      if (toread > ipos) toread = ipos;
      if (toread > opos) toread = opos;
    }
    /* memset (buf, 0, bs); */
    errno = 0; /* should not be necessary */
    do {
      rd += (err = pread (ides, buf+rd, toread-rd, ipos+rd-reverse*toread));
      if (err == -1) rd++;
    } while ((err == -1 && (errno == EINTR || errno == EAGAIN))
	     || (errno == 0 && rd < toread && err > 0));
    /* EOF */
    if (!errno && rd == 0) break;
    if (errno) {
      /* Read error occurred: Print warning */
      printstatus (stdout, log, bs, 1); errs++;
      /* Some errnos are fatal */
      if (errno == ESPIPE) {
	fplog (stderr, "dd_rescue: (warning): %s (%.1fk): %s!\n", 
	       iname, (float)ipos/1024, strerror (errno));
	fplog (stderr, "dd_rescue: Last error fatal! Exiting ...\n");
	cleanup (); exit (20);
      }
      /* Non fatal error */
      if (bs == hardbs) {
	/* Real error: Don't retry */
	nrerr++; 
	fplog (stderr, "dd_rescue: (warning): %s (%.1fk): %s!\n", 
	       iname, (float)ipos/1024, strerror (errno));
	/* exit if too many errs */
	if (maxerr && nrerr >= maxerr) {
	  fplog (stderr, "dd_rescue: (fatal): maxerr reached!\n");
	  cleanup (); exit (32);
	}
	printf ("%s%s%s", down, down, down);
	/* advance */
	fxfer += toread; xfer += toread;
	if (reverse) {ipos -= toread; opos -= toread;}
	else {ipos += toread; opos += toread;}
      } else {
	/* Error with large blocks: Try small ones ... */
	off_t new_max = xfer + toread;
	if (verbose) printf ("dd_rescue: (info): problems at ipos %.1fk: %s \n                 fall back to smaller blocksize \n%s%s%s",
			     (float)ipos/1024, strerror(errno), down, down, down);
	errs += (err = copyfile (new_max, hardbs));
	
	/* Stay with small blocks, until we could read two whole 
	   large ones without errors */
	while (err && (!max || (max-xfer > 0)) && ((!reverse) || (ipos > 0 && opos > 0))) {
	  new_max += 2*softbs;
	  if (new_max > max) new_max = max;
	  errs += (err = copyfile (new_max, hardbs));
	}
	if (verbose) printf ("dd_rescue: (info): ipos %.1fk promote to large bs again! \n%s%s%s",
			     (float)ipos/1024, down, down, down);
	
      } /* bs == hardbs */
    } else {
      /* errno == 0: We can write to disk */
      
      if (rd > 0) {
	ssize_t wr = 0;
	errno = 0; /* should not be necessary */
	if (!sparse || !blockzero (buf, bs))
	  do {
	    wr += (err = pwrite (odes, buf+wr, rd-wr, opos+wr-reverse*toread));
	    if (err == -1) wr++;
	  } while ((err == -1 && (errno == EINTR || errno == EAGAIN))
		   || (errno == 0 && wr < rd && err > 0));
	if (errno) {
	  /* Write error: handle ? .. */
	  fplog (stderr, "dd_rescue: (%s): %s (%.1fk): %s\n",
		 (abwrerr? "fatal": "warning"),
		 oname, (float)opos/1024, strerror (errno));
	  errs++;
	  if (abwrerr) {
		  cleanup (); exit (21);
	  }
	  nrerr++;
	}
	sxfer += wr; xfer += rd;
	if (reverse) {ipos -= rd; opos -= rd;}
	else {ipos += rd; opos += rd;}
	if (rd != wr && !sparse) fplog (stderr, "dd_rescue: (warning): assumption rd(%i) == wr(%i) failed!\n", rd, wr);
      } /* rd > 0 */
    } /* errno */
    if (!quiet && !(xfer % (16*softbs)) && (xfer % (512*softbs))) 
      printstatus (stdout, 0, bs, 0);
    if (!quiet && !(xfer % (512*softbs))) 
      printstatus (stdout, 0, bs, 1);
  } /* remain */
  return errs;
}


off_t readint (char* ptr)
{
  char *es; double res;

  res = strtod (ptr, &es);
  switch (*es) {
  case 'b': res *= 512; break;
  case 'k': res *= 1024; break;
  case 'M': res *= 1024*1024; break;
  case ' ':
  case '\0': break;
  default:
    fplog (stderr, "dd_rescue: (warning): suffix %c ignored!\n", *es);
  }
  return (off_t)res;
}
  
void printversion ()
{
  printf ("\ndd_rescue Version %s, garloff@suse.de, GNU GPL\n", VERSION);
  printf (" (%s)\n", ID);
}

void printhelp ()
{
  printversion ();
  printf ("dd_rescue copies data from one file (or block device) to another\n");
  printf ("USAGE: dd_rescue [options] infile outfile\n");
  printf ("Options: -s ipos    start position in  input file (default=0),\n");
  printf ("         -S opos    start position in output file (def=ipos);\n");
  printf ("         -b softbs  block size for copy operation (def=%i),\n", SOFTBLOCKSIZE );
  printf ("         -b hardbs  fallback block size in case of errs (def=%i);\n", HARDBLOCKSIZE );
  printf ("         -e maxerr  exit after maxerr errors (def=0=infinite);\n");
  printf ("         -m maxxfer maximum amount of data to be transfered (def=0=inf);\n");
  printf ("         -l logfile name of a file to log errors and summary to (def=\"\");\n");
  printf ("         -r         reverse direction copy (def=forward);\n");
  printf ("         -t         truncate output file (def=no);\n");
  printf ("         -w         abort on Write errors (def=no);\n");
  printf ("         -a         spArse file writing (def=no);\n");
  printf ("         -i         interactive: ask before overwriting data (def=no);\n");
  printf ("         -f         force: skip some sanity checks (def=no);\n");
  printf ("         -q         quiet operation,\n");
  printf ("         -v         verbose operation;\n");
  printf ("         -V         display version and exit;\n");
  printf ("         -h         display this help and exit.\n");
  printf ("Note: Sizes may be given in units b(=512), k(=1024) or M(=1024*1024) bytes\n");
  printf ("This program is useful to rescue data in case of I/O errors, because\n");
  printf (" it does not necessarily aborts or truncates the output.\n");
}

#define YESNO(flag) (flag? "yes": "no")

void printinfo (FILE* file)
{
  fplog (file, "dd_rescue: (info): about to transfer %.1f kBytes from %s to %s\n",
	  (double)maxxfer/1024, iname, oname);
  fplog (file, "dd_rescue: (info): blocksizes: soft %i, hard %i\n", softbs, hardbs);
  fplog (file, "dd_rescue: (info): starting positions: in %.1fk, out %.1fk\n",
	  (double)ipos/1024, (double)opos/1024);
  fplog (file, "dd_rescue: (info): Logfile: %s, Maxerr: %li\n",
	  (lname? lname: "(none)"), maxerr);
  fplog (file, "dd_rescue: (info): Reverse: %s, Trunc: %s, interactive: %s\n",
	  YESNO(reverse), YESNO(trunc), YESNO(interact));
  fplog (file, "dd_rescue: (info): abort on Write errs: %s, spArse write: %s\n",
	  YESNO(abwrerr), YESNO(sparse));
  /*
  fplog (file, "dd_rescue: (info): verbose: %s, quiet: %s\n", 
	  YESNO(verbose), YESNO(quiet));
  */
}

void printreport ()
{
  /* report */
  FILE *report = 0;
  if (!quiet || nrerr) report = stdout;
  fplog (report, "Summary for %s -> %s:\n", iname, oname);
  if (report) printf ("%s%s%s", down, down, down);
  if (report) printstatus (stdout, log, 0, 1);
}

void breakhandler (int sig)
{
  fplog (stderr, "dd_rescue: (fatal): Caught signal %i \"%s\". Exiting!\n",
	 sig, strsignal (sig));
  printreport ();
  cleanup ();
  signal (sig, SIG_DFL);
  raise (sig);
}

int main (int argc, char* argv[])
{
  int c;

  /* defaults */
  softbs = SOFTBLOCKSIZE; hardbs = HARDBLOCKSIZE;
  maxerr = 0; ipos = (off_t)-1; opos = (off_t)-1; maxxfer = 0; 
  reverse = 0; trunc = 0; abwrerr = 0; sparse = 0;
  verbose = 0; quiet = 0; interact = 0; force = 0;
  lname = 0; iname = 0; oname = 0;

  /* Initialization */
  sxfer = 0; fxfer = 0; lxfer = 0; xfer = 0;
  ides = -1; odes = -1; log = 0; nrerr = 0; buf = 0;

  while ((c = getopt (argc, argv, ":rtfihqvVwab:B:m:e:s:S:l:")) != -1) {
    switch (c) {
    case 'r': reverse = 1; break;
    case 't': trunc = O_TRUNC; break;
    case 'i': interact = 1; force = 0; break;
    case 'f': interact = 0; force = 1; break;
    case 'a': sparse = 1 ; break;
    case 'w': abwrerr = 1; break;
    case 'h': printhelp (); exit(0); break;
    case 'V': printversion (); exit(0); break;
    case 'v': quiet = 0; verbose = 1; break;
    case 'q': verbose = 0; quiet = 1; break;
    case 'b': softbs = (int)readint (optarg); break;
    case 'B': hardbs = (int)readint (optarg); break;
    case 'm': maxxfer = readint (optarg); break;
    case 'e': maxerr = (int)readint (optarg); break;
    case 's': ipos = readint (optarg); break;
    case 'S': opos = readint (optarg); break;
    case 'l': lname = optarg; break;
    case ':': fplog (stderr, "dd_rescue: (fatal): option %c requires an argument!\n", optopt); 
      printhelp ();
      exit (1); break;
    case '?': fplog (stderr, "dd_rescue: (fatal): unknown option %c!\n", optopt, argv[0]);
      printhelp ();
      exit (1); break;
    default: fplog (stderr, "dd_rescue: (fatal): your getopt() is buggy!\n");
      exit (255);
    }
  }
  
  if (optind < argc) iname = argv[optind++];
  if (optind < argc) oname = argv[optind++];
  if (optind < argc) {
    fplog (stderr, "dd_rescue: (fatal): spurious options: %s ...\n", argv[optind]);
    printhelp ();
    exit (2);
  }
  if (!iname || !oname) {
    fplog (stderr, "dd_rescue: (fatal): both input and output have to be specified!\n");
    printhelp ();
    exit (2);
  }

  if (lname) {
    c = openfile (lname, O_WRONLY | O_CREAT /*| O_EXCL*/);
    log = fdopen (c, "a");
  }

  /* sanity checks */
  if (softbs < hardbs) {
    fplog (stderr, "dd_rescue: (warning): setting hardbs from %i to softbs %i!\n",
	     hardbs, softbs);
    hardbs = softbs;
  }

  if (hardbs <= 0) {
    fplog (stderr, "dd_rescue: (fatal): you're crazy to set you block size to %i!\n", hardbs);
    cleanup (); exit (5);
  }
    
  /* Have those been set by cmdline params? */
  if (ipos == (off_t)-1) ipos = 0;

  buf = malloc (softbs);
  if (!buf) {
    fplog (stderr, "dd_rescue: (fatal): allocation of buffer failed!\n");
    cleanup (); exit (18);
  }
  memset (buf, 0, softbs);

  if (strcmp (iname, oname) == 0 && trunc && !force) {
    fplog (stderr, "dd_rescue: (fatal): infile and outfile are identical and trunc turned on!\n");
    cleanup (); exit (19);
  }
  /* Open input and output files */
  ides = openfile (iname, O_RDONLY);
  if (ides < 0) {
    fplog (stderr, "dd_rescue: (fatal): %s: %s\n", iname, strerror (errno));
    cleanup (); exit (22);
  };
  /* Overwrite? */
  odes = open (oname, O_WRONLY, 0640);
  if (odes > 0 && interact) {
    int a; close (odes);
    do {
      printf ("dd_rescue: (question): %s existing %s [y/n] ?", (trunc? "Overwrite": "Write into"), oname);
      a = toupper (fgetc (stdin)); //printf ("\n");
    } while (a != 'Y' && a != 'N');
    if (a == 'N') {
      fplog (stdout, "dd_rescue: (fatal): exit on user request!\n");
      cleanup (); exit (23);
    }
  }

  odes = openfile (oname, O_WRONLY | O_CREAT /*| O_EXCL*/ | trunc);
  if (odes < 0) {
    fplog (stderr, "dd_rescue: (fatal): %s: %s\n", oname, strerror (errno));
    cleanup (); exit (24);
  };

  /* special case: reverse with ipos == 0 means ipos = end_of_file */
  if (reverse && ipos == 0) {
    ipos = lseek (ides, ipos, SEEK_END);
    if (ipos == -1) {
      fprintf (stderr, "dd_rescue: (fatal): could not seek to end of file %s!\n", iname);
      perror ("dd_rescue"); cleanup (); exit (19);
    }
    if (verbose) 
      fprintf (stderr, "dd_rescue: (info): ipos set to the end: %.1fk\n", 
	       (float)ipos/1024);
    /* if opos not set, assume same position */
    if (opos == (off_t)-1) opos = ipos;
    /* if explicitly set to zero, assume end of _existing_ file */
    if (opos == 0) {
      opos == lseek (odes, opos, SEEK_END);
      if (opos == (off_t)-1) {
	fprintf (stderr, "dd_rescue: (fatal): could not seek to end of file %s!\n", oname);
	perror (""); cleanup (); exit (19);
      }
      /* if existing empty, assume same position */
      if (opos == 0) opos = ipos;
      if (verbose) 
	fprintf (stderr, "dd_rescue: (info): opos set to: %.1fk\n", 
		 (float)opos/1024);
    }
  }
  /* if opos not set, assume same position */
  if (opos == (off_t)-1) opos = ipos;

  if (strcmp (iname, oname) == 0) {
    fplog (stderr, "dd_rescue: (warning): infile and outfile are identical!\n");
    if (opos > ipos && !reverse && !force) {
      fplog (stderr, "dd_rescue: (warning): turned on reverse, as ipos < opos!\n");
      reverse = 1;
    }
    if (opos < ipos && reverse && !force) {
      fplog (stderr, "dd_rescue: (warning): turned off reverse, as opos < ipos!\n");
      reverse = 0;
    }
  }

  if (verbose) {
    printinfo (stdout);
    if (log) printinfo (log);
  }

  /* Install signal handler */
  signal (SIGHUP, breakhandler);
  signal (SIGINT, breakhandler);
  signal (SIGTERM, breakhandler);
  signal (SIGQUIT, breakhandler);
  
  /* Save time and start to work */
  startclock = clock ();
  gettimeofday (&starttime, NULL);
  memcpy (&lasttime, &starttime, sizeof(lasttime));
  if (!quiet) {
    printf ("%s%s%s", down, down, down);
    printstatus (stdout, 0, softbs, 0);
  }

  c = copyfile (maxxfer, softbs);
  
  gettimeofday (&currenttime, NULL);
  printreport ();
  cleanup (); exit (0);
}


      
