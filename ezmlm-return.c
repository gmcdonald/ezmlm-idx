/*$Id: ezmlm-return.c,v 1.26 1999/08/07 20:50:52 lindberg Exp $*/
/*$Name: ezmlm-idx-040 $*/
#include <sys/types.h>
#include "direntry.h"
#include "stralloc.h"
#include "str.h"
#include "env.h"
#include "sig.h"
#include "slurp.h"
#include "getconf.h"
#include "strerr.h"
#include "byte.h"
#include "case.h"
#include "getln.h"
#include "substdio.h"
#include "error.h"
#include "quote.h"
#include "readwrite.h"
#include "fmt.h"
#include "now.h"
#include "cookie.h"
#include "subscribe.h"
#include "errtxt.h"
#include "idx.h"

#define FATAL "ezmlm-return: fatal: "
#define INFO "ezmlm-return: info: "
void die_usage()
{ strerr_die1x(100,"ezmlm-return: usage: ezmlm-return [-dD] dir"); }
void die_nomem() { strerr_die2x(111,FATAL,ERR_NOMEM); }
void die_badaddr()
{
  strerr_die2x(100,FATAL,ERR_BAD_RETURN_ADDRESS);
}
void die_trash()
{
  strerr_die2x(99,INFO,"trash address");
}

char outbuf[1024];
substdio ssout;
char inbuf[1024];
substdio ssin;

char strnum[FMT_ULONG];
char hash[COOKIE];
char hashcopy[COOKIE];
char *hashp = (char *) 0;
unsigned long cookiedate;
unsigned long addrno = 0L;
unsigned long addrno1 = 0L;
stralloc fndir = {0};
stralloc fndate = {0};
stralloc fndatenew = {0};
stralloc fnhash = {0};
stralloc fnhashnew = {0};
void *psql = (void *) 0;

stralloc quoted = {0};
stralloc ddir = {0};
char *sender;
char *dir;
char *workdir;

void die_hashnew()
{ strerr_die4sys(111,FATAL,ERR_WRITE,fnhashnew.s,": "); }
void die_datenew()
{ strerr_die4sys(111,FATAL,ERR_WRITE,fndatenew.s,": "); }
void die_msgin()
{ strerr_die2sys(111,FATAL,ERR_READ_INPUT); }

void makedir(s)
char *s;
{
  if (mkdir(s,0755) == -1)
    if (errno != error_exist)
      strerr_die4x(111,FATAL,ERR_CREATE,s,": ");
}

void dowit(addr,when,bounce)
char *addr;
unsigned long when;
stralloc *bounce;
{
  int fd;
  unsigned int wpos;
  unsigned long wdir,wfile;

  if (!issub(workdir,addr,(char *) 0,FATAL)) return;

  if (!stralloc_copys(&fndate,workdir)) die_nomem();
  if (!stralloc_cats(&fndate,"/bounce/d")) die_nomem();
  if (!stralloc_0(&fndate)) die_nomem();
  fndate.s[fndate.len - 1] = '/';	/* replace '\0' */
  wpos = fndate.len - 1;
  wdir = when / 10000;
  wfile = when - 10000 * wdir;
  if (!stralloc_catb(&fndate,strnum,fmt_ulong(strnum,wdir))) die_nomem();
  if (!stralloc_0(&fndate)) die_nomem();
  makedir(fndate.s);
  --fndate.len;				/* remove terminal '\0' */
  if (!stralloc_cats(&fndate,"/w")) die_nomem();
  wpos = fndate.len - 1;
  if (!stralloc_catb(&fndate,strnum,fmt_ulong(strnum,wfile))) die_nomem();
  if (!stralloc_cats(&fndate,".")) die_nomem();
  if (!stralloc_catb(&fndate,strnum,fmt_ulong(strnum,(unsigned long) getpid())))
	die_nomem();
  if (!stralloc_0(&fndate)) die_nomem();
  if (!stralloc_copy(&fndatenew,&fndate)) die_nomem();
  fndatenew.s[wpos] = 'W';

  fd = open_trunc(fndatenew.s);
  if (fd == -1) die_datenew();
  substdio_fdbuf(&ssout,write,fd,outbuf,sizeof(outbuf));
  if (substdio_puts(&ssout,addr) == -1) die_datenew();
  if (substdio_put(&ssout,"",1) == -1) die_datenew();
  if (substdio_puts(&ssout,"Return-Path: <") == -1) die_datenew();
  if (!quote2(&quoted,sender)) die_nomem();
  if (substdio_put(&ssout,quoted.s,quoted.len) == -1) die_datenew();
  if (substdio_puts(&ssout,">\n") == -1) die_datenew();
  if (substdio_put(&ssout,bounce->s,bounce->len) == -1) die_datenew();
  if (substdio_flush(&ssout) == -1) die_datenew();
  if (fsync(fd) == -1) die_datenew();
  if (close(fd) == -1) die_datenew(); /* NFS stupidity */

  if (rename(fndatenew.s,fndate.s) == -1)
    strerr_die6sys(111,FATAL,ERR_MOVE,fndatenew.s," to ",fndate.s,": ");
}

void doit(addr,msgnum,when,bounce)
char *addr;
unsigned long msgnum;
unsigned long when;
stralloc *bounce;
{
  int fd;
  int fdnew;
  unsigned int pos;
  unsigned long ddir,dfile;

  if (!issub(workdir,addr,(char *) 0,FATAL)) return;

  if (!stralloc_copys(&fndate,workdir)) die_nomem();
  if (!stralloc_cats(&fndate,"/bounce/d")) die_nomem();
  if (!stralloc_0(&fndate)) die_nomem();
  makedir(fndate.s);
  fndate.s[fndate.len-1] = '/';		/* replace terminal '\0' */
  ddir = when / 10000;
  dfile = when - 10000 * ddir;
  if (!stralloc_catb(&fndate,strnum,fmt_ulong(strnum,ddir))) die_nomem();
  if (!stralloc_copy(&fndir,&fndate)) die_nomem();
  if (!stralloc_0(&fndir)) die_nomem();	/* make later if necessary (new addr)*/
  if (!stralloc_cats(&fndate,"/d")) die_nomem();
  pos = fndate.len - 2;
  if (!stralloc_catb(&fndate,strnum,fmt_ulong(strnum,dfile))) die_nomem();
  if (!stralloc_cats(&fndate,".")) die_nomem();
  if (!stralloc_catb(&fndate,strnum,fmt_ulong(strnum,(unsigned long) getpid())))
	 die_nomem();
  if (addrno) {	/* so that pre-VERP bounces make a d... file per address */
		/* for the first one we use the std-style fname */
    if (!stralloc_cats(&fndate,".")) die_nomem();
    if (!stralloc_catb(&fndate,strnum,fmt_ulong(strnum,addrno))) die_nomem();
  }
  addrno++;	/* get ready for next */
  if (!stralloc_0(&fndate)) die_nomem();
  if (!stralloc_copy(&fndatenew,&fndate)) die_nomem();
  fndatenew.s[pos] = '_';	/* fndate = bounce/d/nnnn/dmmmmmm */
				/* fndatenew = bounce/d/nnnn_dmmmmmm */

  fd = open_trunc(fndatenew.s);
  if (fd == -1) die_datenew();
  substdio_fdbuf(&ssout,write,fd,outbuf,sizeof(outbuf));
  if (substdio_puts(&ssout,addr) == -1) die_datenew();
  if (substdio_put(&ssout,"",1) == -1) die_datenew();
  if (substdio_puts(&ssout,"Return-Path: <") == -1) die_datenew();
  if (!quote2(&quoted,sender)) die_nomem();
  if (substdio_put(&ssout,quoted.s,quoted.len) == -1) die_datenew();
  if (substdio_puts(&ssout,">\n") == -1) die_datenew();
  if (substdio_put(&ssout,bounce->s,bounce->len) == -1) die_datenew();
  if (substdio_flush(&ssout) == -1) die_datenew();
  if (fsync(fd) == -1) die_datenew();
  if (close(fd) == -1) die_datenew(); /* NFS stupidity */

  cookie(hash,"",0,"",addr,"");
  if (!stralloc_copys(&fnhash,workdir)) die_nomem();
  if (!stralloc_cats(&fnhash,"/bounce/h")) die_nomem();
  if (!stralloc_0(&fnhash)) die_nomem();
  makedir(fnhash.s);
  fnhash.s[fnhash.len - 1] = '/';		/* replace terminal '\0' */
  if (!stralloc_catb(&fnhash,hash,1)) die_nomem();
  if (!stralloc_0(&fnhash)) die_nomem();
  makedir(fnhash.s);
  --fnhash.len;					/* remove terminal '\0' */
  if (!stralloc_cats(&fnhash,"/h")) die_nomem();
  pos = fnhash.len - 1;
  if (!stralloc_catb(&fnhash,hash+1,COOKIE-1)) die_nomem();
  if (!stralloc_0(&fnhash)) die_nomem();
  if (!stralloc_copy(&fnhashnew,&fnhash)) die_nomem();
  fnhashnew.s[pos] = 'H';

  fdnew = open_trunc(fnhashnew.s);
  if (fdnew == -1) die_hashnew();
  substdio_fdbuf(&ssout,write,fdnew,outbuf,sizeof(outbuf));

  fd = open_read(fnhash.s);
  if (fd == -1) {
    if (errno != error_noent)
      strerr_die4sys(111,FATAL,ERR_READ,fnhash.s,": ");
    makedir(fndir.s);
    if (rename(fndatenew.s,fndate.s) == -1)
      strerr_die6sys(111,FATAL,ERR_MOVE,fndatenew.s," to ",fndate.s,": ");
  }
  else {
    substdio_fdbuf(&ssin,read,fd,inbuf,sizeof(inbuf));
    switch(substdio_copy(&ssout,&ssin)) {
      case -2: die_msgin();
      case -3: die_hashnew();
    }
    close(fd);
    if (unlink(fndatenew.s) == -1)
      strerr_die4sys(111,FATAL,ERR_DELETE,fndatenew.s,": ");
  }
  if (substdio_puts(&ssout,"   ") == -1) die_hashnew();
  if (substdio_put(&ssout,strnum,fmt_ulong(strnum,msgnum)) == -1) die_hashnew();
  if (substdio_puts(&ssout,"\n") == -1) die_hashnew();
  if (substdio_flush(&ssout) == -1) die_hashnew();
  if (fsync(fdnew) == -1) die_hashnew();
  if (close(fdnew) == -1) die_hashnew(); /* NFS stupidity */

  if (rename(fnhashnew.s,fnhash.s) == -1)
    strerr_die6sys(111,FATAL,ERR_MOVE,fnhashnew.s," to ",fnhash.s,": ");
}

stralloc bounce = {0};
stralloc line = {0};
stralloc header = {0};
stralloc intro = {0};
stralloc failure = {0};
stralloc paragraph = {0};
int flagmasterbounce = 0;
int flaghaveheader;
int flaghaveintro;

stralloc key = {0};

char msginbuf[1024];
substdio ssmsgin;

void main(argc,argv)
int argc;
char **argv;
{
  char *local;
  char *action;
  char *def;
  char *ret;
  char *cp;
  unsigned long msgnum;
  unsigned long cookiedate;
  unsigned long when;
  unsigned long listno = 0L;
  int match;
  unsigned int i;
  int flagdig = 0;
  int flagmaster = 0;
  int flagreceipt = 0;
  int fdlock;
  register char ch;

  umask(022);
  sig_pipeignore();
  when = (unsigned long) now();

  dir = argv[1];
  if (!dir) die_usage();
  if (*dir == '-') {			/* for normal use */
    if (dir[1] == 'd') {
      flagdig = 1;
    } else if (dir[1] == 'D') {
      flagdig = 0;
    } else
      die_usage();
    dir = argv[2];
    if (!dir) die_usage();
  }

  sender = env_get("SENDER");
  if (!sender) strerr_die2x(100,FATAL,ERR_NOSENDER);
  local = env_get("LOCAL");
  if (!local) strerr_die2x(100,FATAL,ERR_NOLOCAL);
  def = env_get("DEFAULT");		/* qmail-1.02 */

  if (chdir(dir) == -1)
    strerr_die4sys(111,FATAL,ERR_SWITCH,dir,": ");

  switch(slurp("key",&key,32)) {
    case -1:
      strerr_die4sys(111,FATAL,ERR_READ,dir,"/key: ");
    case 0:
      strerr_die4x(100,FATAL,dir,"/key",ERR_NOEXIST);
  }
  workdir = dir;
  action = def;

    if (str_start(action,"receipt-")) {
      flagreceipt = 1;
      action += 8;
    }
    ch = *action;		/* -d -digest, -m -master, -g -getmaster */
    if (ch && action[1] == '-') {
      switch (ch) {
	case 'g': flagmaster = 1; flagdig = 1; action += 2; break;
	case 'm': flagmaster = 1; action += 2; break;
	default: break;
      }
    }
  if (flagdig) {
    if (!stralloc_copys(&ddir,dir)) die_nomem();
    if (!stralloc_cats(&ddir,"/digest")) die_nomem();
    if (!stralloc_0(&ddir)) die_nomem();
    workdir = ddir.s;
  }

  if (!*action) die_trash();

  if (flagreceipt || flagmaster)			/* check cookie */
    if (str_chr(action,'-') == COOKIE) {
      action[COOKIE] = '\0';
      hashp = action;
      action += COOKIE + 1;
  }

  if (!flagreceipt) {
    if (!flagmaster && str_start(action,"probe-")) {
      action += 6;
      action += scan_ulong(action,&cookiedate);
      if (now() - cookiedate > 3000000) die_trash();
      if (*action++ != '.') die_trash();
      i = str_chr(action,'-');
      if (i != COOKIE) die_trash();
      byte_copy(hashcopy,COOKIE,action);
      action += COOKIE;
      if (*action++ != '-') die_trash();
      i = str_rchr(action,'=');
      if (!stralloc_copyb(&line,action,i)) die_nomem();
      if (action[i]) {
	if (!stralloc_cats(&line,"@")) die_nomem();
	if (!stralloc_cats(&line,action + i + 1)) die_nomem();
      }
      if (!stralloc_0(&line)) die_nomem();
      strnum[fmt_ulong(strnum,cookiedate)] = 0;
      cookie(hash,key.s,key.len,strnum,line.s,"P");
      if (byte_diff(hash,COOKIE,hashcopy)) die_trash();

      (void) subscribe(workdir,line.s,0,"","-probe",1,-1,(char *) 0,FATAL);
      _exit(99);
    }

    if (!stralloc_copys(&line,workdir)) die_nomem();
    if (!stralloc_cats(&line,"/lockbounce")) die_nomem();
    if (!stralloc_0(&line)) die_nomem();

    fdlock = open_append(line.s);
    if (fdlock == -1)
      strerr_die4sys(111,FATAL,ERR_OPEN,line.s,": ");
    if (lock_ex(fdlock) == -1)
      strerr_die4sys(111,FATAL,ERR_OBTAIN,line.s,": ");

    if (!flagmaster && str_start(action,"warn-")) {
      action += 5;
      action += scan_ulong(action,&cookiedate);
      if (now() - cookiedate > 3000000) die_trash();
      if (*action++ != '.') die_trash();
      i = str_chr(action,'-');
      if (i != COOKIE) die_trash();
      byte_copy(hashcopy,COOKIE,action);
      action += COOKIE;
      if (*action++ != '-') die_trash();
      i = str_rchr(action,'=');
      if (!stralloc_copyb(&line,action,i)) die_nomem();
      if (action[i]) {
        if (!stralloc_cats(&line,"@")) die_nomem();
        if (!stralloc_cats(&line,action + i + 1)) die_nomem();
      }
      if (!stralloc_0(&line)) die_nomem();
      strnum[fmt_ulong(strnum,cookiedate)] = 0;
      cookie(hash,key.s,key.len,strnum,line.s,"W");
      if (byte_diff(hash,COOKIE,hashcopy)) die_trash();

      if (slurpclose(0,&bounce,1024) == -1) die_msgin();
      dowit(line.s,when,&bounce);
      _exit(99);
    }
  }
  action += scan_ulong(action,&msgnum);
  if (*action++ != '-') die_badaddr();
  cp = action;
  if (*action >= '0' && *action <= '9') {		/* listno */
    action += scan_ulong(action,&listno);
    listno++;					/* logging is 1-53, not 0-52 */
  }

  if (hashp) {		/* scrap bad cookies */
      if ((ret = checktag(workdir,msgnum,0L,"x",(char *) 0,hashp))) {
        if (*ret)
	  strerr_die2x(111,FATAL,*ret);
	else
	  die_trash();
      } else if (flagreceipt) {
	if (!(ret = logmsg(dir,msgnum,listno,0L,5))) {
	  closesql();
	  strerr_die6x(99,INFO,"receipt:",cp," [",hashp,"]");
	}
	if (*ret) strerr_die2x(111,FATAL,ret);
	else strerr_die2x(0,INFO,ERR_DONE);
      } else if (*action) {	/* post VERP master bounce */
	if ((ret = logmsg(dir,msgnum,listno,0L,-1))) {
	  closesql();
	  strerr_die4x(0,INFO,"bounce [",hashp,"]");
	}
	if (*ret) strerr_die2x(111,FATAL,ret);
	else strerr_die2x(99,INFO,ERR_DONE);
      }
   } else if (flagreceipt || flagmaster)
	die_badaddr();

  if (*action) {
    i = str_rchr(action,'=');
    if (!stralloc_copyb(&line,action,i)) die_nomem();
    if (action[i]) {
      if (!stralloc_cats(&line,"@")) die_nomem();
      if (!stralloc_cats(&line,action + i + 1)) die_nomem();
    }
    if (!stralloc_0(&line)) die_nomem();
    if (slurpclose(0,&bounce,1024) == -1) die_msgin();
    doit(line.s,msgnum,when,&bounce);
    _exit(99);
  }

  /* pre-VERP bounce, in QSBMF format. Receipts are never pre-VERP */

  substdio_fdbuf(&ssmsgin,read,0,msginbuf,sizeof(msginbuf));

  flaghaveheader = 0;
  flaghaveintro = 0;

  for (;;) {
    if (!stralloc_copys(&paragraph,"")) die_nomem();
    for (;;) {
      if (getln(&ssmsgin,&line,&match,'\n') == -1) die_msgin();
      if (!match) die_trash();
      if (!stralloc_cat(&paragraph,&line)) die_nomem();
      if (line.len <= 1) break;
    }

    if (!flaghaveheader) {
      if (!stralloc_copy(&header,&paragraph)) die_nomem();
      flaghaveheader = 1;
      continue;
    }

    if (!flaghaveintro) {
      if (paragraph.s[0] == '-' && paragraph.s[1] == '-')
        continue;		/* skip MIME boundary if it exists */
      if (paragraph.len < 15) die_trash();
      if (str_diffn(paragraph.s,"Hi. This is the",15)) die_trash();
      if (!stralloc_copy(&intro,&paragraph)) die_nomem();
      flaghaveintro = 1;
      continue;
    }

    if (paragraph.s[0] == '-')
      break;

    if (paragraph.s[0] == '<') {
      if (!stralloc_copy(&failure,&paragraph)) die_nomem();

      if (!stralloc_copy(&bounce,&header)) die_nomem();
      if (!stralloc_cat(&bounce,&intro)) die_nomem();
      if (!stralloc_cat(&bounce,&failure)) die_nomem();

      i = byte_chr(failure.s,failure.len,'\n');
      if (i < 3) die_trash();

      if (!stralloc_copyb(&line,failure.s + 1,i - 3)) die_nomem();
      if (byte_chr(line.s,line.len,'\0') == line.len) {
        if (!stralloc_0(&line)) die_nomem();
        if (flagmaster) {				/* bounced msg slave! */
	  if ((i = str_rchr(line.s,'@')) >= 5) {	/* 00_52@host */
	    line.s[i] = '\0';				/* 00_52 */
	    (void) scan_ulong(line.s + i - 5,&listno);
	      if ((ret = logmsg(dir,msgnum,listno + 1,0L,-1)) && *ret)
		strerr_die2x(111,FATAL,ret);
	    strerr_warn3(INFO,"bounce ",line.s + i - 5,(struct strerr *) 0);
	    flagmasterbounce = 1;
	  }
	} else
	  doit(line.s,msgnum,when,&bounce);
      }
    }
  }
  closesql();
  if (flagmasterbounce)
    strerr_die3x(0,"[",hashp,"]");
  else
    _exit(99);
}