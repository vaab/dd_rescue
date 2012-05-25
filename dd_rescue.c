/* dd_rescue.c */
/* 
 * dd_rescue copies your data from one file to another.
 * Files might as well be block devices, such as hd partitions.
 * Unlike dd, it does not necessarily abort on errors but
 * continues to copy the disk, possibly leaving holes behind.
 * Also, it does NOT truncate the output file, so you can copy 
 * more and more pieces of your data, as time goes by.
 * This tool is thus suitable for rescueing data of crashed disk,
 * and that's the reason it has been written by me.
 *
 * (c) Kurt Garloff <garloff@suse.de>, 11/97, 10/99
 * Copyright: GNU GPL
 *
 * Improvements from LAB Valentin, see
 * http://www.tharbad.ath.cx/~vaab/kalysto/Utilities/dd_rhelp/dd_rhelp_en.html
 */

/*
 * TODO:
 * - Use termcap to fetch cursor up/down codes
 * - Better handling of write errors: also try sub blocks
 */

#ifndef VERSION
# define VERSION "(unknown)"
#endif

#define ID "$Id: dd_rescue.c,v 1.59 2007/08/26 13:42:44 garloff Exp $"

#ifndef SOFTBLOCKSIZE
# define SOFTBLOCKSIZE 65536
#endif

#ifndef HARDBLOCKSIZE
# define HARDBLOCKSIZE 512
#endif

#define _GNU_SOURCE
#define _LARGEFILE_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <getopt.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <utime.h>
#include <sys/time.h>
#include <sys/stat.h>

int softbs, hardbs, syncfreq;
int maxerr, nrerr, reverse, dotrunc, abwrerr, sparse, nosparse;
int verbose, quiet, interact, force;
void* buf;
char *lname, *iname, *oname, *bbname = NULL;
off_t ipos, opos, xfer, lxfer, sxfer, fxfer, maxxfer, init_opos;

int ides, odes, identical, pres;
int o_dir_in, o_dir_out;
char i_chr, o_chr;

FILE *logfd;
struct timeval starttime, lasttime, currenttime;
struct timezone tz;
clock_t startclock;

#ifndef UP
# define UP "\x1b[A"
# define DOWN "\n"
# define RIGHT "\x1b[C"
#endif

const char* up = UP;
const char* down = DOWN;
const char* right = RIGHT;

int lastsparse = 0;

inline float difftimetv(const struct timeval* const t2, 
			const struct timeval* const t1)
{
	return  (float) (t2->tv_sec  - t1->tv_sec ) +
		(float) (t2->tv_usec - t1->tv_usec) * 1e-6;
}

/* Write to file and simultaneously log to logfdile, if exsiting */
int fplog(FILE* const file, const char * const fmt, ...)
{
	int ret = 0;
	va_list vl; 
	va_start(vl, fmt);
	if (file) 
		ret = vfprintf(file, fmt, vl);
	va_end(vl);
	if (logfd) {
		va_start(vl, fmt);
		ret = vfprintf(logfd, fmt, vl);
		va_end(vl);
	}
	return ret;
}

int check_identical(const char* const in, const char* const on)
{
	int err = 0;
	struct stat istat, ostat;
	errno = 0;
	if (strcmp(in, on) == 0) 
		return 1;
	err -= stat(in, &istat);
	if (err)
	       	return 0;
	err -= stat(on, &ostat);
	errno = 0;
	if (!err &&
	    istat.st_ino == ostat.st_ino &&
	    istat.st_dev == ostat.st_dev)
		return 1;
	return 0;
}


int openfile(const char* const fname, const int flags)
{
	int fdes;
	if (!strcmp(fname, "-")) {
		if (flags & O_WRONLY) 
			fdes = 1;  /* stdout */
		else 
			fdes = 0;  /* stdin */
	} else
		fdes = open(fname, flags, 0640);
	if (fdes == -1) {
		char nbuf[128];
		snprintf(nbuf, 128, "dd_rescue: (fatal): open \"%s\" failed", fname);
		perror(nbuf); exit(17);
	}
	return fdes;
}

/* Checks whether files are seekable */
void check_seekable(const int id, const int od)
{
	errno = 0;
	if (lseek(id, (off_t)0, SEEK_SET) != 0) {
		fplog(stderr, "dd_rescue: (warning): input  file is not seekable!\n");
		fplog(stderr, "dd_rescue: (warning): %s\n", strerror(errno));
		i_chr = 1;
	} //else
	//	lseek(id, (off_t)0, SEEK_SET);
	errno = 0;
	if (lseek(od, (off_t)0, SEEK_SET) != 0) {
		fplog(stderr, "dd_rescue: (warning): output file is not seekable!\n");
		fplog(stderr, "dd_rescue: (warning): %s\n", strerror(errno));
		o_chr = 1;
	} //else
	//	lseek(od, (off_t)0, SEEK_SET);
	errno = 0;
}


void doprint(FILE* const file, const int bs, const clock_t cl, 
	     const float t1, const float t2, const int sync)
{
	fprintf(file, "dd_rescue: (info): ipos:%12.1fk, opos:%12.1fk, xferd:%12.1fk\n",
		(float)ipos/1024, (float)opos/1024, (float)xfer/1024);
	fprintf(file, "             %s  %s  errs:%7i, errxfer:%12.1fk, succxfer:%12.1fk\n",
		(reverse? "-": " "), (bs==hardbs? "*": " "), nrerr, 
		(float)fxfer/1024, (float)sxfer/1024);
	if (sync || (file != stdin && file != stdout) )
		fprintf(file, "             +curr.rate:%9.0fkB/s, avg.rate:%9.0fkB/s, avg.load:%5.1f%%\n",
			(float)(xfer-lxfer)/(t2*1024),
			(float)xfer/(t1*1024),
			100.0*(cl-startclock)/(CLOCKS_PER_SEC*t1));
	else
		fprintf(file, "             -curr.rate:%s%s%s%s%s%s%s%s%skB/s, avg.rate:%9.0fkB/s, avg.load:%5.1f%%\n",
			right, right, right, right, right, right, right, right, right,
			(float)xfer/(t1*1024),
			100.0*(cl-startclock)/(CLOCKS_PER_SEC*t1));
}

void printstatus(FILE* const file1, FILE* const file2,
		 const int bs, const int sync)
{
	float t1, t2; 
	clock_t cl;

	if (file1 == stderr || file1 == stdout) 
		fprintf(file1, "%s%s%s", up, up, up);
	if (file2 == stderr || file2 == stdout) 
		fprintf(file2, "%s%s%s", up, up, up);

	if (sync) {
		int err = fsync(odes);
		if (err)
			fplog(stderr, "dd_rescue: (warning): %s (%.1fk): %s!    \n", 
			      oname, (float)ipos/1024, strerror(errno));
	}

	gettimeofday(&currenttime, NULL);
	t1 = difftimetv(&currenttime, &starttime);
	t2 = difftimetv(&currenttime, &lasttime);
	cl = clock();

	if (file1) 
		doprint(file1, bs, cl, t1, t2, sync);
	if (file2)
		doprint(file2, bs, cl, t1, t2, sync);
	if (sync) {
		memcpy(&lasttime, &currenttime, sizeof(lasttime));
		lxfer = xfer;
	}
}

void savebb(int block)
{
	FILE *bbfile;
	fplog(stderr, "Bad block: %d\n", block);
	if (bbname == NULL)
		return;
	bbfile = fopen(bbname, "a");
	fprintf(bbfile, "%d\n", block);
	fclose(bbfile);
}

void printreport()
{
	/* report */
	FILE *report = (!quiet || nrerr)? stderr: 0;
	fplog(report, "Summary for %s -> %s:\n", iname, oname);
	if (report)
		fprintf(stderr, "%s%s%s", down, down, down);
	printstatus(report, logfd, 0, 1);
}

int mayexpandfile()
{	
	struct stat st;
	off_t maxopos = opos;
	if (init_opos > opos)
		maxopos = init_opos;
	stat(oname, &st);
	if (!S_ISREG(st.st_mode))
		return 0;
	if (st.st_size < maxopos)
		return truncate(oname, maxopos);
	else 
		return 0;		
}

int cleanup()
{
	int rc, errs = 0;
	if (odes != -1) {
		/* Make sure, the output file is expanded to the last (first) position
	 	 * FIXME: 0 byte writes do NOT expand file */
		pwrite(odes, buf, 0, opos);
		rc = fsync(odes);
		if (rc) {
			fplog(stderr, "dd_rescue: (warning): %s (%.1fk): %s!\n", 
			      oname, (float)opos/1024, strerror(errno));
			++errs;
		}
		rc = close(odes); 
		if (rc) {
			fplog(stderr, "dd_rescue: (warning): %s (%.1fk): %s!\n", 
			      oname, (float)opos/1024, strerror(errno));
			++errs;
		}
		if (sparse)
			rc = mayexpandfile();
			if (rc)
				fplog(stderr, "dd_rescue: (warning): %s (%1.fk): %s!\n",
				      oname, (float)opos/1024, strerror(errno));
	}
	if (ides != -1) {
		rc = close(ides);
		if (rc) {
			fplog(stderr, "dd_rescue: (warning): %s (%.1fk): %s!\n", 
			      iname, (float)ipos/1024, strerror(errno));
			++errs;
		}
	}
	if (logfd)
		fclose(logfd);
	if (buf)
		free(buf);
	return errs;
}

/* is the block zero ? */
int blockiszero(const char* blk, const int ln)
{
	unsigned long* ptr = (unsigned long*)blk;
	while ((ptr-(unsigned long*)blk) < ln/sizeof(unsigned long))
		if (*(ptr++)) 
			return (sizeof(unsigned long)*(ptr-(unsigned long*)blk));
	return ln;
}

inline ssize_t mypread(int fd, void* bf, size_t sz, off_t off)
{
	if (i_chr) 
		return read(fd, bf, sz);
	else
		return pread(fd, bf, sz, off);
}

inline ssize_t mypwrite(int fd, void* bf, size_t sz, off_t off)
{
	if (o_chr)
		return write(fd, bf, sz);
	else
		return pwrite(fd, bf, sz, off);
}


ssize_t readblock(const int toread)
{
	ssize_t err, rd = 0;
	//errno = 0; /* should not be necessary */
	do {
		rd += (err = mypread(ides, buf+rd, toread-rd, ipos+rd-reverse*toread));
		if (err == -1) 
			rd++;
	} while ((err == -1 && (errno == EINTR || errno == EAGAIN))
		  || (rd < toread && err > 0 && errno == 0));
	//if (rd < toread) memset (buf+rd, 0, toread-rd);
	return (/*err == -1? err:*/ rd);
}

ssize_t writeblock(const int towrite)
{
	ssize_t err, wr = 0;
	//errno = 0; /* should not be necessary */
	do {
		wr += (err = mypwrite(odes, buf+wr, towrite-wr, opos+wr-reverse*towrite));
		if (err == -1) 
			wr++;
	} while ((err == -1 && (errno == EINTR || errno == EAGAIN))
		  || (wr < towrite && err > 0 && errno == 0));
	if (wr < towrite && err != 0) {
		/* Write error: handle ? .. */
		fplog(stderr, "dd_rescue: (%s): %s (%.1fk): %s\n",
		      (abwrerr? "fatal": "warning"),
		      oname, (float)opos/1024, strerror(errno));
		if (abwrerr) {
			cleanup(); exit(21);
		}
		nrerr++;
	}
	return (/*err == -1? err:*/ wr);
}

/* can be invoked in two ways: bs==hardbs or bs==softbs */
int copyfile(const off_t max, const int bs)
{
	int errs = 0; errno = 0;
#if 0	
	fprintf(stderr, "%s%s%s copyfile (ipos=%.1fk, xfer=%.1fk, max=%.1fk, bs=%i)                         ##\n%s%s%s",
		up, up, up,
		(float)ipos/1024, (float)xfer/1024, (float)max/1024, bs,
		down, down, down);
#endif
	/* expand file to AT LEAST the right length 
	 * FIXME: 0 byte writes do NOT expand file */
	if (!o_chr)
		pwrite(odes, buf, 0, opos);
	while ( (!max || (max-xfer > 0))
		&& ((!reverse) || (ipos > 0 && opos > 0)) ) {
		int err;
		ssize_t rd = 0;
		ssize_t toread = ((max && max-xfer < bs)? (max-xfer): bs);
		if (reverse) {
			if (toread > ipos) 
				toread = ipos;
			if (toread > opos)
				toread = opos;
		}

		if (nosparse && bs == hardbs)
			memset(buf, 0, bs);
		rd = readblock(toread);

		/* EOF */
		if (rd == 0 && !errno) {
			if (!errs)
				fplog(stderr, "dd_rescue: (info): %s (%.1fk): EOF\n", 
				      iname, (float)ipos/1024);
			return errs;
		}
		/* READ ERROR */
		if (rd < toread/* && errno*/) {
			/* Read error occurred: Print warning */
			printstatus(stderr, logfd, bs, 1); 
			errs++;
			/* Some errnos are fatal */
			if (errno == ESPIPE || errno == EPERM || errno == ENXIO || errno == ENODEV) {
				fplog(stderr, "dd_rescue: (warning): %s (%.1fk): %s!\n", 
				      iname, (float)ipos/1024, strerror(errno));
				fplog(stderr, "dd_rescue: Last error fatal! Exiting ...\n");
				cleanup(); exit(20);
			}
			/* Non fatal error */
			if (bs == hardbs) {
				/* Real error: Don't retry */
				nrerr++; 
				fplog(stderr, "dd_rescue: (warning): %s (%.1fk): %s!\n", 
				      iname, (float)ipos/1024, strerror(errno));
				/* exit if too many errs */
				if (maxerr && nrerr >= maxerr) {
					fplog(stderr, "dd_rescue: (fatal): maxerr reached!\n");
					printreport();
					cleanup(); exit(32);
				}
				fprintf(stderr, "%s%s%s", down, down, down);
				/* advance */
				errno = 0;
				if (nosparse) {
					ssize_t wr = 0;
					errs += ((wr = writeblock(toread)) < toread ? 1: 0);
					lastsparse = 0;
					if (wr < 0 && (errno == ENOSPC 
						   || (errno == EFBIG && !reverse))) 
						return errs;
					if (toread != wr) {
						fplog(stderr, "dd_rescue: (warning): assumption toread(%i) == wr(%i) failed! \n", toread, wr);	
						/*
						fplog(stderr, "dd_rescue: (warning): %s (%.1fk): %s!\n", 
						      oname, (float)opos/1024, strerror(errno));
						fprintf(stderr, "%s%s%s", down, down, down);
					 	*/
					}
				}
				savebb(ipos/bs);
				fxfer += toread; xfer += toread;
				if (reverse) { 
					ipos -= toread; opos -= toread; 
				} else { 
					ipos += toread; opos += toread; 
				}
			} else {
				off_t new_max = xfer + toread;
				off_t old_xfer;
				/* Error with large blocks: Try small ones ... */
				if (verbose) 
					fprintf(stderr, "dd_rescue: (info): problems at ipos %.1fk: %s \n                 fall back to smaller blocksize \n%s%s%s",
					        (float)ipos/1024, strerror(errno), down, down, down);
				/* But first: write available data and advance (optimization) */
				if (rd > 0) {
					ssize_t wr = 0; errno = 0;
					if (!sparse || blockiszero(buf, bs) < rd) {
						errs += ((wr = writeblock(rd)) < rd ? 1: 0);
						lastsparse = 0;
					} else
						lastsparse = 1;

					if (!reverse) {
						ipos += rd; opos += rd; 
						sxfer += wr; xfer += rd;
					}
					/* 
					else {
						new_max -= rd;
					}
					 */
					if (wr < 0 && (errno == ENOSPC 
						   || (errno == EFBIG && !reverse))) 
						return errs;
					if (rd != wr && !sparse) {
						fplog(stderr, "dd_rescue: (warning): assumption rd(%i) == wr(%i) failed! \n", rd, wr);
						/*
						fplog(stderr, "dd_rescue: (warning): %s (%.1fk): %s!\n", 
						      oname, (float)opos/1024, strerror(errno));
						fprintf(stderr, "%s%s%s", down, down, down);
					 	 */
					}
				} /* rd > 0 */
				old_xfer = xfer;
				errs += (err = copyfile(new_max, hardbs));
				/* EOF */
				if (!err && old_xfer == xfer)
					return errs;
				/*
				if (reverse && rd) {
					ipos -= rd; opos -= rd;
					xfer += rd; sxfer += wr;
				}
				*/	
				/* Stay with small blocks, until we could read two whole 
				   large ones without errors */
				new_max = xfer;
				while (err && (!max || (max-xfer > 0)) && ((!reverse) || (ipos > 0 && opos > 0))) {
					new_max += 2*softbs; old_xfer = xfer;
					if (max && new_max > max) 
						new_max = max;
					errs += (err = copyfile(new_max, hardbs));
				}
				errno = 0;
				/* EOF ? */      
				if (!err && xfer == old_xfer)
					return errs;
				if (verbose) 
					fprintf(stderr, "dd_rescue: (info): ipos %.1fk promote to large bs again! \n%s%s%s",
						(float)ipos/1024, down, down, down);
			} /* bs == hardbs */
		} else {
	      		/* errno == 0: We can write to disk */
      			if (rd > 0) {
				ssize_t wr = 0;
				if (!sparse || blockiszero(buf, bs) < rd) {
					errs += ((wr = writeblock(rd)) < rd ? 1: 0);
					lastsparse = 0;
				} else
					lastsparse = 1;
				sxfer += wr; xfer += rd;
				if (reverse) { 
					ipos -= rd; opos -= rd; 
				} else { 
					ipos += rd; opos += rd; 
				}
				if (wr < 0 && (errno == ENOSPC 
					   || (errno == EFBIG && !reverse))) 
					return errs;
				if (rd != wr && !sparse) {
					fplog(stderr, "dd_rescue: (warning): assumption rd(%i) == wr(%i) failed! \n", rd, wr);
					fplog(stderr, "dd_rescue: (warning): %s (%.1fk): %s!\n", 
					      oname, (float)opos/1024, strerror(errno));
					fprintf(stderr, "%s%s", down, down);
					errno = 0;
				}
			} /* rd > 0 */
		} /* errno */

		if (syncfreq && !(xfer % (syncfreq*softbs)))
			printstatus((quiet? 0: stderr), 0, bs, 1);
		else if (!quiet && !(xfer % (16*softbs)))
			printstatus(stderr, 0, bs, 0);
	} /* remain */
	return errs;
}

int copyperm(int ifd, int ofd)
{
	int err; 
	mode_t fmode;
	struct stat stbuf;
	err = fstat(ifd, &stbuf);
	if (err)
		return err;
	fmode = stbuf.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO | S_ISUID | S_ISGID | S_ISVTX);
	err = fchown(ofd, stbuf.st_uid, stbuf.st_gid);
	if (err)
		fmode &= ~(S_ISUID | S_ISGID);
	err += fchmod(ofd, fmode);
	return err;
}

/*  File time copy */
int copytimes(const char* inm, const char* onm)
{
	int err;
	struct stat stbuf;
	struct utimbuf utbuf;
	err = stat(inm, &stbuf);
	if (err)
		return err;
	utbuf.actime  = stbuf.st_atime;
	utbuf.modtime = stbuf.st_mtime;
	err = utime(onm, &utbuf);
	return err;
}

off_t readint(const char* const ptr)
{
	char *es; double res;

	res = strtod(ptr, &es);
	switch (*es) {
		case 'b': res *= 512; break;
		case 'k': res *= 1024; break;
		case 'M': res *= 1024*1024; break;
		case 'G': res *= 1024*1024*1024; break;
		case ' ':
		case '\0': break;
		default:
			fplog(stderr, "dd_rescue: (warning): suffix %c ignored!\n", *es);
	}
	return (off_t)res;
}

void printversion()
{
	fprintf(stderr, "\ndd_rescue Version %s, garloff@suse.de, GNU GPL\n", VERSION);
	fprintf(stderr, " (%s)\n", ID);
}

void printhelp()
{
	printversion();
	fprintf(stderr, "dd_rescue copies data from one file (or block device) to another.\n");
	fprintf(stderr, "USAGE: dd_rescue [options] infile outfile\n");
	fprintf(stderr, "Options: -s ipos    start position in  input file (default=0),\n");
	fprintf(stderr, "         -S opos    start position in output file (def=ipos),\n");
	fprintf(stderr, "         -b softbs  block size for copy operation (def=%i),\n", SOFTBLOCKSIZE );
	fprintf(stderr, "         -B hardbs  fallback block size in case of errs (def=%i),\n", HARDBLOCKSIZE );
	fprintf(stderr, "         -e maxerr  exit after maxerr errors (def=0=infinite),\n");
	fprintf(stderr, "         -m maxxfer maximum amount of data to be transfered (def=0=inf),\n");
	fprintf(stderr, "         -y syncfrq frequency of fsync calls on outfile (def=512*softbs),\n");
	fprintf(stderr, "         -l logfile name of a file to log errors and summary to (def=\"\"),\n");
	fprintf(stderr, "         -o bbfile  name of a file to log bad blocks numbers (def=\"\"),\n");
	fprintf(stderr, "         -r         reverse direction copy (def=forward),\n");
	fprintf(stderr, "         -t         truncate output file (def=no),\n");
#ifdef O_DIRECT
	fprintf(stderr, "         -d/D       use O_DIRECT for input/output (def=no),\n");
#endif
	fprintf(stderr, "         -w         abort on Write errors (def=no),\n");
	fprintf(stderr, "         -a         spArse file writing (def=no),\n");
	fprintf(stderr, "         -A         Always write blocks, zeroed if err (def=no),\n");
	fprintf(stderr, "         -i         interactive: ask before overwriting data (def=no),\n");
	fprintf(stderr, "         -f         force: skip some sanity checks (def=no),\n");
	fprintf(stderr, "         -p         preserve: preserve ownership / perms (def=no),\n");
	fprintf(stderr, "         -q         quiet operation,\n");
	fprintf(stderr, "         -v         verbose operation,\n");
	fprintf(stderr, "         -V         display version and exit,\n");
	fprintf(stderr, "         -h         display this help and exit.\n");
	fprintf(stderr, "Note: Sizes may be given in units b(=512), k(=1024), M(=1024^2) or G(1024^3) bytes\n");
	fprintf(stderr, "This program is useful to rescue data in case of I/O errors, because\n");
	fprintf(stderr, " it does not necessarily abort or truncate the output.\n");
}

#define YESNO(flag) (flag? "yes": "no ")

void printinfo(FILE* const file)
{
	fplog(file, "dd_rescue: (info): about to transfer %.1f kBytes from %s to %s\n",
	      (double)maxxfer/1024, iname, oname);
	fplog(file, "dd_rescue: (info): blocksizes: soft %i, hard %i\n", softbs, hardbs);
	fplog(file, "dd_rescue: (info): starting positions: in %.1fk, out %.1fk\n",
	      (double)ipos/1024, (double)opos/1024);
	fplog(file, "dd_rescue: (info): Logfile: %s, Maxerr: %li\n",
	      (lname? lname: "(none)"), maxerr);
	fplog(file, "dd_rescue: (info): Reverse: %s, Trunc: %s, interactive: %s\n",
	      YESNO(reverse), YESNO(dotrunc), YESNO(interact));
	fplog(file, "dd_rescue: (info): abort on Write errs: %s, spArse write: %s\n",
	      YESNO(abwrerr), (sparse? "yes": (nosparse? "never": "if err")));
	/*
	fplog(file, "dd_rescue: (info): verbose: %s, quiet: %s\n", 
	      YESNO(verbose), YESNO(quiet));
	*/
}

void breakhandler(int sig)
{
	fplog(stderr, "dd_rescue: (fatal): Caught signal %i \"%s\". Exiting!\n",
	      sig, strsignal(sig));
	printreport();
	cleanup();
	signal(sig, SIG_DFL);
	raise(sig);
}

int main(int argc, char* argv[])
{
	int c;
	off_t syncsz = -1;
	void **mp = (void **) &buf;

  	/* defaults */
	softbs = SOFTBLOCKSIZE; hardbs = HARDBLOCKSIZE;
	maxerr = 0; ipos = (off_t)-1; opos = (off_t)-1; maxxfer = 0; 
	reverse = 0; dotrunc = 0; abwrerr = 0; sparse = 0; nosparse = 0;
	verbose = 0; quiet = 0; interact = 0; force = 0; pres = 0;
	lname = 0; iname = 0; oname = 0; o_dir_in = 0; o_dir_out = 0;

	/* Initialization */
	sxfer = 0; fxfer = 0; lxfer = 0; xfer = 0;
	ides = -1; odes = -1; logfd = 0; nrerr = 0; buf = 0;
	i_chr = 0; o_chr = 0;

	while ((c = getopt(argc, argv, ":rtfihqvVwaAdDpb:B:m:e:s:S:l:o:y:")) != -1) {
		switch (c) {
			case 'r': reverse = 1; break;
			case 't': dotrunc = O_TRUNC; break;
			case 'i': interact = 1; force = 0; break;
			case 'f': interact = 0; force = 1; break;
#ifdef O_DIRECT
			case 'd': o_dir_in  = O_DIRECT; break;
			case 'D': o_dir_out = O_DIRECT; break;
#endif
			case 'p': pres = 1; break;
			case 'a': sparse = 1; nosparse = 0; break;
			case 'A': nosparse = 1; sparse = 0; break;
			case 'w': abwrerr = 1; break;
			case 'h': printhelp(); exit(0); break;
			case 'V': printversion(); exit(0); break;
			case 'v': quiet = 0; verbose = 1; break;
			case 'q': verbose = 0; quiet = 1; break;
			case 'b': softbs = (int)readint(optarg); break;
			case 'B': hardbs = (int)readint(optarg); break;
			case 'm': maxxfer = readint(optarg); break;
			case 'e': maxerr = (int)readint(optarg); break;
			case 'y': syncsz = readint(optarg); break;
			case 's': ipos = readint(optarg); break;
			case 'S': opos = readint(optarg); break;
			case 'l': lname = optarg; break;
			case 'o': bbname = optarg; break;
			case ':': fplog (stderr, "dd_rescue: (fatal): option %c requires an argument!\n", optopt); 
				printhelp();
				exit(11); break;
			case '?': fplog(stderr, "dd_rescue: (fatal): unknown option %c!\n", optopt, argv[0]);
				printhelp();
				exit(11); break;
			default: fplog(stderr, "dd_rescue: (fatal): your getopt() is buggy!\n");
				exit(255);
		}
	}
  
	init_opos = opos;
	if (optind < argc) 
		iname = argv[optind++];
	if (optind < argc) 
		oname = argv[optind++];
	if (optind < argc) {
		fplog(stderr, "dd_rescue: (fatal): spurious options: %s ...\n", argv[optind]);
		printhelp();
		exit(12);
	}
	if (!iname || !oname) {
		fplog(stderr, "dd_rescue: (fatal): both input and output have to be specified!\n");
		printhelp();
		exit(12);
	}

	if (lname) {
		c = openfile(lname, O_WRONLY | O_CREAT /*| O_EXCL*/);
		logfd = fdopen(c, "a");
	}

	/* sanity checks */
#ifdef O_DIRECT
	if ((o_dir_in || o_dir_out) && hardbs < sysconf(_SC_PAGESIZE)) {
		hardbs = sysconf(_SC_PAGESIZE);
		fplog(stderr, "dd_rescue: (warning): O_DIRECT requires hardbs of at least %i!\n",
		      hardbs);
	}

	if (o_dir_in || o_dir_out)
		fplog(stderr, "dd_rescue: (warning): We don't handle misalignment of last block w/ O_DIRECT!\n");
				
#endif				

	if (softbs < hardbs) {
		fplog(stderr, "dd_rescue: (warning): setting hardbs from %i to softbs %i!\n",
		      hardbs, softbs);
		hardbs = softbs;
	}

	if (hardbs <= 0) {
		fplog(stderr, "dd_rescue: (fatal): you're crazy to set block size to %i!\n", hardbs);
		cleanup(); exit(15);
	}

	/* Set sync frequency */
	if (syncsz == -1)
		syncfreq = 512;
	else if (syncsz == 0)
		syncfreq = 0;
	else
		syncfreq = (syncsz + softbs - 1) / softbs;

	/* Have those been set by cmdline params? */
	if (ipos == (off_t)-1) 
		ipos = 0;

#ifdef O_DIRECT
	if (posix_memalign(mp, sysconf(_SC_PAGESIZE), softbs)) {
		fplog(stderr, "dd_rescue: (fatal): allocation of aligned buffer failed!\n");
		cleanup(); exit(18);
	}
#else
	buf = malloc(softbs);
	if (!buf) {
		fplog(stderr, "dd_rescue: (fatal): allocation of buffer failed!\n");
		cleanup(); exit(18);
	}
#endif

	memset(buf, 0, softbs);

	identical = check_identical(iname, oname);
	if (identical && dotrunc && !force) {
		fplog(stderr, "dd_rescue: (fatal): infile and outfile are identical and trunc turned on!\n");
		cleanup(); exit(19);
	}
	/* Open input and output files */
	ides = openfile(iname, O_RDONLY | o_dir_in);
	if (ides < 0) {
		fplog(stderr, "dd_rescue: (fatal): %s: %s\n", iname, strerror(errno));
		cleanup(); exit(22);
	}

	/* Overwrite? */
	/* Special case '-': stdout */
	if (strcmp(oname, "-"))
		odes = open(oname, O_WRONLY | o_dir_out, 0640);
	else 
		odes = 0;

	if (odes > 0) 
		close(odes);

	if (odes > 0 && interact) {
		int a;
		do {
			fprintf(stderr, "dd_rescue: (question): %s existing %s [y/n] ?", 
				(dotrunc? "Overwrite": "Write into"), oname);
			a = toupper(fgetc (stdin)); //fprintf(stderr, "\n");
		} while (a != 'Y' && a != 'N');
		if (a == 'N') {
			fplog(stderr, "dd_rescue: (fatal): exit on user request!\n");
			cleanup(); exit(23);
		}
	}

	odes = openfile(oname, O_WRONLY | O_CREAT | o_dir_out /*| O_EXCL*/ | dotrunc);
	if (odes < 0) {
		fplog(stderr, "dd_rescue: (fatal): %s: %s\n", oname, strerror(errno));
		cleanup(); exit(24);
	}

	if (pres)
		copyperm(ides, odes);
			
	check_seekable(ides, odes);

	if (0 && i_chr && o_chr) {
		fprintf(stderr, "dd_rescue: (fatal): Sorry, there is no support yet for non-seekable\n");
		fprintf(stderr, "                    input and output. This will hopefully change soon ... \n");
		exit(19);
	}

	if (o_chr) {
		if (!nosparse)
			fprintf(stderr, "dd_rescue: (warning): Don't use sparse writes for non-seekable output\n");
		nosparse = 1; sparse = 0;
	}

	/* special case: reverse with ipos == 0 means ipos = end_of_file */
	if (reverse && ipos == 0) {
		ipos = lseek(ides, ipos, SEEK_END);
		if (ipos == -1) {
			fprintf(stderr, "dd_rescue: (fatal): could not seek to end of file %s!\n", iname);
			perror("dd_rescue"); cleanup(); exit(19);
		}
		if (verbose) 
			fprintf(stderr, "dd_rescue: (info): ipos set to the end: %.1fk\n", 
			        (float)ipos/1024);
		/* if opos not set, assume same position */
		if (opos == (off_t)-1) 
			opos = ipos;
		/* if explicitly set to zero, assume end of _existing_ file */
		if (opos == 0) {
			opos = lseek(odes, opos, SEEK_END);
			if (opos == (off_t)-1) {
				fprintf(stderr, "dd_rescue: (fatal): could not seek to end of file %s!\n", oname);
				perror(""); cleanup(); exit(19);
			}
			/* if existing empty, assume same position */
			if (opos == 0) 
				opos = ipos;
			if (verbose) 
				fprintf(stderr, "dd_rescue: (info): opos set to: %.1fk\n",
					(float)opos/1024);
    		}
	}

	/* if opos not set, assume same position */
	if (opos == (off_t)-1) 
		opos = ipos;

	if (identical) {
		fplog(stderr, "dd_rescue: (warning): infile and outfile are identical!\n");
		if (opos > ipos && !reverse && !force) {
			fplog(stderr, "dd_rescue: (warning): turned on reverse, as ipos < opos!\n");
			reverse = 1;
    		}
		if (opos < ipos && reverse && !force) {
			fplog(stderr, "dd_rescue: (warning): turned off reverse, as opos < ipos!\n");
			reverse = 0;
		}
  	}

	if (o_chr && opos != 0) {
		fplog(stderr, "dd_rescue: (fatal): outfile not seekable, but opos !=0 requested!\n");
		cleanup(); exit(19);
	}
	if (i_chr && ipos != 0) {
		fplog(stderr, "dd_rescue: (fatal): infile not seekable, but ipos !=0 requested!\n");
		cleanup(); exit(19);
	}
		

	if (verbose) {
		printinfo(stderr);
		if (logfd)
			printinfo(logfd);
	}

	/* Install signal handler */
	signal(SIGHUP , breakhandler);
	signal(SIGINT , breakhandler);
	signal(SIGTERM, breakhandler);
	signal(SIGQUIT, breakhandler);

	/* Save time and start to work */
	startclock = clock();
	gettimeofday(&starttime, NULL);
	memcpy(&lasttime, &starttime, sizeof(lasttime));

	if (!quiet) {
		fprintf(stderr, "%s%s%s", down, down, down);
		printstatus(stderr, 0, softbs, 0);
	}

	c = copyfile(maxxfer, softbs);
	gettimeofday(&currenttime, NULL);
	printreport();
	cleanup();
	if (pres)
		copytimes(iname, oname);
	return c;
}

