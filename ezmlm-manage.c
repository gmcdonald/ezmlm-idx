/*$Id: ezmlm-manage.c,v 1.86 1999/12/23 02:43:55 lindberg Exp $*/
/*$Name: ezmlm-idx-040 $*/

#include <sys/types.h>
#include <sys/stat.h>
#include "error.h"
#include "stralloc.h"
#include "str.h"
#include "env.h"
#include "sig.h"
#include "slurp.h"
#include "getconf.h"
#include "strerr.h"
#include "byte.h"
#include "getln.h"
#include "case.h"
#include "qmail.h"
#include "substdio.h"
#include "readwrite.h"
#include "seek.h"
#include "quote.h"
#include "datetime.h"
#include "now.h"
#include "date822fmt.h"
#include "fmt.h"
#include "subscribe.h"
#include "cookie.h"
#include "sgetopt.h"
#include "copy.h"
#include "errtxt.h"
#include "idx.h"

#define FATAL "ezmlm-manage: fatal: "
#define INFO "ezmlm-manage: info: "

int flagverbose = 0;	/* default: Owner not informed about subdb changes */
			/* 1 => notified for failed unsub, 2 => for all */
int flagnotify = 1;	/* notify subscriber of completed events. 0 also */
			/* suppresses all subscriber communication for */
			/* [un]sub if -U/-S is used */
int flagbottom = 1;	/* default: copy request & admin info to message */
int flaglist = 0;	/* default: do not reply to -list */
int flagget = 1;	/* default: service -get requests */
int flagsubconf = 1;	/* default: require user-confirm for subscribe */
int flagunsubconf = 1;	/* default: require user-confirm for unsubscribe */
int flagunsubismod = 0;	/* default: do not require moderator approval to */
			/* unsubscribe from moderated list */
int flagedit = 0;	/* default: text file edit not allowed */
int flagstorefrom = 1;	/* default: store from: line for subscribes */
char flagcd = '\0';	/* default: do not use _Q_uoted printable or _B_ase64 */
char encin = '\0';	/* encoding of incoming message */
int flagdig = 0;	/* request is not for digest list */

static const char hex[]="0123456789ABCDEF";
char urlstr[] = "%00";	/* to build a url-encoded version of a char */

int act = AC_NONE;	/* desired action */
unsigned int actlen = 0;/* str_len of above */
char *dir;
char *workdir;
char *sender;
void *psql = (void *) 0;

void die_usage() {
  strerr_die1x(100,"ezmlm-manage: usage: ezmlm-manage "
		"[-bBcCdDeEfFlLmMnNqQsSuUvV] dir"); }

void die_nomem() { strerr_die2x(111,FATAL,ERR_NOMEM); }

void die_badaddr()
{
  strerr_die2x(100,FATAL,ERR_BAD_ADDRESS);
}

void die_cookie()
{
  strerr_die2x(100,FATAL,ERR_MOD_COOKIE);
}

stralloc inhost = {0};
stralloc outhost = {0};
stralloc inlocal = {0};
stralloc outlocal = {0};
stralloc key = {0};
stralloc mailinglist = {0};
stralloc mydtline = {0};
stralloc target = {0};
stralloc verptarget = {0};
stralloc confirm = {0};
stralloc line = {0};
stralloc qline = {0};
stralloc quoted = {0};
stralloc moddir = {0};
stralloc ddir = {0};
stralloc modsub = {0};
stralloc remote = {0};
stralloc from = {0};
stralloc to = {0};
stralloc owner = {0};
stralloc fromline = {0};
stralloc text = {0};
stralloc fnedit = {0};
stralloc fneditn = {0};
stralloc charset = {0};

datetime_sec when;
struct datetime dt;
int match;
unsigned int max;

char strnum[FMT_ULONG];
char date[DATE822FMT];
char hash[COOKIE];
char boundary[COOKIE];
datetime_sec hashdate;

char inbuf[1024];
substdio ssin = SUBSTDIO_FDBUF(read,0,inbuf,(int) sizeof(inbuf));
substdio ssin2 = SUBSTDIO_FDBUF(read,0,inbuf,(int) sizeof(inbuf));

substdio sstext;	/* editing texts and reading "from" */
char textbuf[512];

substdio ssfrom;	/* writing "from" */
char frombuf[512];

int fdlock;

void lock()
{
    fdlock = open_append("lock");
    if (fdlock == -1)
      strerr_die4sys(111,FATAL,ERR_OPEN,dir,"/lock: ");
    if (lock_ex(fdlock) == -1)
      strerr_die4sys(111,FATAL,ERR_OBTAIN,dir,"/lock: ");
}

void unlock()
{
    close(fdlock);
}

void make_verptarget()
/* puts target with '=' instead of last '@' into stralloc verptarget */
/* and does set_cpverptarget */
{
  unsigned int i;

  i = str_rchr(target.s,'@');
  if (!stralloc_copyb(&verptarget,target.s,i)) die_nomem();
  if (target.s[i]) {
    if (!stralloc_append(&verptarget,"=")) die_nomem();
    if (!stralloc_cats(&verptarget,target.s + i + 1)) die_nomem();
  }
  if (!stralloc_0(&verptarget)) die_nomem();
  set_cpverptarget(verptarget.s);
}

void store_from(frl,adr)
/* rewrites the from file removing all that is older than 1000000 secs  */
/* and add the curent from line (frl). Forget it if there is none there.*/
/* NOTE: This is used only for subscribes to moderated lists!           */
stralloc *frl;	/* from line */
char *adr;
{
  int fdin;
  int fdout;
  unsigned long linetime;

  if (!flagstorefrom || !frl->len) return;	/* nothing to store */
  lock();
  if ((fdout = open_trunc("fromn")) == -1)
    strerr_die3sys(111,FATAL,ERR_OPEN,"fromn: ");
  substdio_fdbuf(&ssfrom,write,fdout,frombuf,(int) sizeof(frombuf));
  if ((fdin = open_read("from")) == -1) {
    if (errno != error_noent)
      strerr_die3sys(111,FATAL,ERR_OPEN,"from: ");
  } else {
      substdio_fdbuf(&sstext,read,fdin,textbuf,(int) sizeof(textbuf));
      for (;;) {
	if (getln(&sstext,&line,&match,'\n') == -1)
	strerr_die3sys(111,FATAL,ERR_READ,"from: ");
	if (!match) break;
	(void) scan_ulong(line.s,&linetime);
	if (linetime + 1000000 > when && linetime <= when)
	  if (substdio_bput(&ssfrom,line.s,line.len))
	    strerr_die3sys(111,FATAL,ERR_WRITE,"fromn: ");
      }
      close(fdin);
  }					/* build new entry */
  if (!stralloc_copyb(&line,strnum,fmt_ulong(strnum,when))) die_nomem();
  if (!stralloc_append(&line," ")) die_nomem();
  if (!stralloc_cats(&line,adr)) die_nomem();
  if (!stralloc_0(&line)) die_nomem();
  if (!stralloc_catb(&line,frl->s,frl->len)) die_nomem();
  if (!stralloc_append(&line,"\n")) die_nomem();
  if (substdio_bput(&ssfrom,line.s,line.len) == -1)
    strerr_die3sys(111,FATAL,ERR_WRITE,"fromn: ");
  if (substdio_flush(&ssfrom) == -1)
    strerr_die3sys(111,FATAL,ERR_WRITE,"fromn: ");
  if (fsync(fdout) == -1)
    strerr_die3sys(111,FATAL,ERR_SYNC,"fromn: ");
  if (close(fdout) == -1)
    strerr_die3sys(111,FATAL,ERR_CLOSE,"fromn: ");
  if (rename("fromn","from") == -1)
    strerr_die3sys(111,FATAL,ERR_MOVE,"from: ");
  unlock();
}

char *get_from(adr,act)
/* If we captured a from line, it will be from the subscriber, except   */
/* when -S is used when it's usually from the subscriber, but of course */
/* could be from anyone. The matching to stored data is required only   */
/* to support moderated lists, and in cases where a new -sc is issued   */
/* because an old one was invalid. In this case, we read through the    */
/* from file trying to match up a timestamp with that starting in       */
/* *(act+3). If the time stamp matches, we compare the target address   */
/* itself. act + 3 must be a legal part of the string returns pointer to*/
/* fromline, NULL if not found. Since the execution time from when to   */
/* storage may differ, we can't assume that the timestamps are in order.*/

char *adr;		/* target address */
char *act;		/* action */
{
  int fd;
  char *fl;
  unsigned int pos;
  unsigned long thistime;
  unsigned long linetime;

  if (!flagstorefrom) return 0;
  if (fromline.len) {	/* easy! We got it in this message */
    if (!stralloc_0(&fromline)) die_nomem(FATAL);
    return fromline.s;
  }			/* need to recover it from DIR/from */
  fl = 0;
  (void) scan_ulong(act+3,&thistime);
  if ((fd = open_read("from")) == -1)
    if (errno == error_noent)
      return 0;
    else
      strerr_die3x(111,FATAL,ERR_READ,"from: ");
  substdio_fdbuf(&sstext,read,fd,textbuf,(int) sizeof(textbuf));
  for (;;) {
    if (getln(&sstext,&fromline,&match,'\n') == -1)
      strerr_die3sys(111,FATAL,ERR_READ,"from: ");
    if (!match) break;
    fromline.s[fromline.len - 1] = (char) 0;
	/* now:time addr\0fromline\0 read all. They can be out of order! */
    pos = scan_ulong(fromline.s,&linetime);
    if (linetime != thistime) continue;
    if (!str_diff(fromline.s + pos + 1,adr)) {
      pos = str_len(fromline.s);
      if (pos < fromline.len) {
	fl = fromline.s + pos + 1;
	break;
      }
    }
  }
  close(fd);
  return fl;
}

int hashok(action,ac)
char *action;
char *ac;
{
  char *x;
  datetime_sec u;

  x = action + 3;
  x += scan_ulong(x,&u);
  hashdate = u;
  if (hashdate > when) return 0;
  if (hashdate < when - 1000000) return 0;

  u = hashdate;
  strnum[fmt_ulong(strnum,(unsigned long) u)] = 0;
  cookie(hash,key.s,key.len - flagdig,strnum,target.s,ac);

  if (*x == '.') ++x;
  if (str_len(x) != COOKIE) return 0;
  return byte_equal(hash,COOKIE,x);
}

struct qmail qq;
int qqwrite(fd,buf,len) int fd; char *buf; unsigned int len;
{
  qmail_put(&qq,buf,len);
  return len;
}

char qqbuf[1];
substdio ssqq = SUBSTDIO_FDBUF(qqwrite,-1,qqbuf,(int) sizeof(qqbuf));

int code_qput(s,n)
char *s;
unsigned int n;
{
    if (!flagcd)
      qmail_put(&qq,s,n);
    else {
      if (flagcd == 'B')
        encodeB(s,n,&qline,0,FATAL);
      else
        encodeQ(s,n,&qline,FATAL);
      qmail_put(&qq,qline.s,qline.len);
    }
    return 0;		/* always succeeds */
}

int subto(s,l)
char *s;
unsigned int l;
{
  qmail_put(&qq,"T",1);
  qmail_put(&qq,s,l);
  qmail_put(&qq,"",1);
  return (int) l;
}

int code_subto(s,l)
char *s;
unsigned int l;
{
  code_qput(s,l);
  code_qput("\n",1);
  return (int) l;
}

int dummy_to(s,l)
char *s;	/* ignored */
unsigned int l;
{
  return (int) l;
}

void transferenc()
{
	if (flagcd) {
          qmail_puts(&qq,"\n--");
          qmail_put(&qq,boundary,COOKIE);
          qmail_puts(&qq,"\nContent-Type: text/plain; charset=");
          qmail_puts(&qq,charset.s);
	  qmail_puts(&qq,"\nContent-Transfer-Encoding: ");
          if (flagcd == 'Q')
            qmail_puts(&qq,"quoted-printable\n\n");
          else
	    qmail_puts(&qq,"base64\n\n");
        } else
          qmail_puts(&qq,"\n");
}

void to_owner()
{
	if (!stralloc_copy(&owner,&outlocal)) die_nomem();
	if (!stralloc_cats(&owner,"-owner@")) die_nomem();
	if (!stralloc_cat(&owner,&outhost)) die_nomem();
	if (!stralloc_0(&owner)) die_nomem();
	qmail_to(&qq,owner.s);
}

void mod_bottom()
{
      copy(&qq,"text/mod-sub",flagcd,FATAL);
      copy(&qq,"text/bottom",flagcd,FATAL);
      code_qput(TXT_SUPPRESSED,str_len(TXT_SUPPRESSED));
      if (flagcd) {
        qmail_puts(&qq,"\n--");
        qmail_put(&qq,boundary,COOKIE);
        qmail_puts(&qq,"--\n");
      }
      if (flagcd == 'B') {
        encodeB("",0,&line,2,FATAL);	/* flush */
        qmail_put(&qq,line.s,line.len);
      }
      qmail_from(&qq,from.s);
}
void msg_headers()
		/* Writes all the headers up to but not including subject */
{
  int flaggoodfield;
  int flagfromline;
  int flaggetfrom;
  unsigned int pos;

  qmail_puts(&qq,"Mailing-List: ");
  qmail_put(&qq,mailinglist.s,mailinglist.len);
  if(getconf_line(&line,"listid",0,FATAL,dir)) {
    qmail_puts(&qq,"\nList-ID: ");
    qmail_put(&qq,line.s,line.len);
  }
  if (!quote(&quoted,&outlocal)) die_nomem();	/* quoted has outlocal */
  qmail_puts(&qq,"\nList-Help: <mailto:");	/* General rfc2369 headers */
  qmail_put(&qq,quoted.s,quoted.len);
  qmail_puts(&qq,"-help@");
  qmail_put(&qq,outhost.s,outhost.len);
  qmail_puts(&qq,">\nList-Post: <mailto:");
  qmail_put(&qq,quoted.s,quoted.len);
  qmail_puts(&qq,"@");
  qmail_put(&qq,outhost.s,outhost.len);
  qmail_puts(&qq,">\nList-Subscribe: <mailto:");
  qmail_put(&qq,quoted.s,quoted.len);
  qmail_puts(&qq,"-subscribe@");
  qmail_put(&qq,outhost.s,outhost.len);
  qmail_puts(&qq,">\nDate: ");
  datetime_tai(&dt,when);
  qmail_put(&qq,date,date822fmt(date,&dt));
  qmail_puts(&qq,"Message-ID: <");
  if (!stralloc_copyb(&line,strnum,fmt_ulong(strnum,(unsigned long) when)))
     die_nomem();
  if (!stralloc_append(&line,".")) die_nomem();
  if (!stralloc_catb(&line,strnum,
		fmt_ulong(strnum,(unsigned long) getpid()))) die_nomem();
  if (!stralloc_cats(&line,".ezmlm@")) die_nomem();
  if (!stralloc_cat(&line,&outhost)) die_nomem();
  if (!stralloc_0(&line)) die_nomem();
  qmail_puts(&qq,line.s);
		/* "unique" MIME boundary as hash of messageid */
  cookie(boundary,"",0,"",line.s,"");
  qmail_puts(&qq,">\nFrom: ");
  qmail_put(&qq,quoted.s,quoted.len);
  if (act == AC_HELP)		/* differnt "From:" for help to break auto- */
    qmail_puts(&qq,"-return-@");	/* responder loops */
  else
    qmail_puts(&qq,"-help@");
  qmail_put(&qq,outhost.s,outhost.len);
  qmail_puts(&qq,"\nTo: ");
  if (!quote2(&quoted,target.s)) die_nomem();
  qmail_put(&qq,quoted.s,quoted.len);
  qmail_puts(&qq,"\n");
  if (!stralloc_copys(&mydtline,"Delivered-To: responder for ")) die_nomem();
  if (!stralloc_catb(&mydtline,outlocal.s,outlocal.len)) die_nomem();
  if (!stralloc_cats(&mydtline,"@")) die_nomem();
  if (!stralloc_catb(&mydtline,outhost.s,outhost.len)) die_nomem();
  if (!stralloc_cats(&mydtline,"\n")) die_nomem();
  qmail_put(&qq,mydtline.s,mydtline.len);

  flaggoodfield = 0;
  flagfromline = 0;
	/* do it for -sc, but if the -S flag is used, do it for -subscribe */
  flaggetfrom = flagstorefrom &&
	 ((act == AC_SC) || ((act == AC_SUBSCRIBE) && !flagsubconf));
  for (;;) {
    if (getln(&ssin,&line,&match,'\n') == -1)
      strerr_die2sys(111,FATAL,ERR_READ_INPUT);
    if (!match) break;
    if (line.len == 1) break;
    if ((line.s[0] != ' ') && (line.s[0] != '\t')) {
      flagfromline = 0;
      flaggoodfield = 0;
      if (case_startb(line.s,line.len,"mailing-list:"))
        strerr_die2x(100,FATAL,ERR_MAILING_LIST);
      if (line.len == mydtline.len)
	if (byte_equal(line.s,line.len,mydtline.s))
          strerr_die2x(100,FATAL,ERR_LOOPING);
      if (case_startb(line.s,line.len,"delivered-to:"))
        flaggoodfield = 1;
      else if (case_startb(line.s,line.len,"received:"))
        flaggoodfield = 1;
      else if (case_startb(line.s,line.len,"content-transfer-encoding:")) {
        pos = 26;
        while (line.s[pos] == ' ' || line.s[pos] == '\t') ++pos;
        if (case_startb(line.s+pos,line.len-pos,"base64"))
          encin = 'B';
        else if (case_startb(line.s+pos,line.len-pos,"quoted-printable"))
          encin = 'Q';
      } else if (flaggetfrom && case_startb(line.s,line.len,"from:")) {
	flagfromline = 1;		/* for logging subscriber data */
	pos = 5;
	while (line.s[pos] == ' ' || line.s[pos] == '\t') ++pos;
        if (!stralloc_copyb(&fromline,line.s + pos,line.len - pos - 1))
	  die_nomem();
      }
    } else {
      if (flagfromline == 1)		/* scrap terminal '\n' */
        if (!stralloc_catb(&fromline,line.s,line.len - 1)) die_nomem();
    }
    if (flaggoodfield)
      qmail_put(&qq,line.s,line.len);
  }
  qmail_puts(&qq,"MIME-Version: 1.0\n");
  if (flagcd) {
    qmail_puts(&qq,"Content-Type: multipart/mixed; charset=");
    qmail_puts(&qq,charset.s);
    qmail_puts(&qq,";\n\tboundary=");
    qmail_put(&qq,boundary,COOKIE);
  } else {
    qmail_puts(&qq,"Content-type: text/plain; charset=");
    qmail_puts(&qq,charset.s);
  }
  qmail_puts(&qq,"\n");
}

int geton(action)
char *action;
{
  char *fl;
  int r;
  unsigned int i;
  unsigned char ch;

  fl = get_from(target.s,action);		/* try to match up */
  switch((r = subscribe(workdir,target.s,1,fl,"+",1,-1,(char *) 0,FATAL))) {
    case 1:
	    qmail_puts(&qq,"List-Unsubscribe: <mailto:");	/*rfc2369 */
	    qmail_put(&qq,outlocal.s,outlocal.len);
	    qmail_puts(&qq,"-unsubscribe-");
		/* url-encode since verptarget is controlled by sender */
		/* note &verptarget ends in '\0', hence len - 1! */
	    for (i = 0; i < verptarget.len - 1; i++) {
	      ch = verptarget.s[i];
	      if (str_chr("\"?;<>&/:%+#",ch) < 10 ||
			 (ch <= ' ') || (ch & 0x80)) {
		urlstr[1] = hex[ch / 16];
	        urlstr[2] = hex[ch & 0xf];
		qmail_put(&qq,urlstr,3);
	      } else {
		qmail_put(&qq,verptarget.s + i, 1);
	      }
	    }
	    qmail_puts(&qq,"@");
	    qmail_put(&qq,outhost.s,outhost.len);	/* safe */
	    qmail_puts(&qq,">\n");
            qmail_puts(&qq,TXT_WELCOME);
	    if (!quote(&quoted,&outlocal)) die_nomem();
            qmail_put(&qq,quoted.s,quoted.len);
            qmail_puts(&qq,"@");
            qmail_put(&qq,outhost.s,outhost.len);
            qmail_puts(&qq,"\n");
            transferenc();
	    if (!stralloc_copy(&confirm,&outlocal)) die_nomem();
	    if (!stralloc_append(&confirm,"unsubscribe-")) die_nomem();
	    if (!stralloc_cats(&confirm,verptarget.s)) die_nomem();
	    if (!stralloc_append(&confirm,"@")) die_nomem();
	    if (!stralloc_cat(&confirm,&outhost)) die_nomem();
	    if (!stralloc_0(&confirm)) die_nomem();
	    set_cpconfirm(confirm.s);			/* for !R in copy */
            copy(&qq,"text/top",flagcd,FATAL);
            copy(&qq,"text/sub-ok",flagcd,FATAL);
            break;
    default:
            if (str_start(action,ACTION_TC))
              strerr_die2x(0,INFO,ERR_SUB_NOP);
            qmail_puts(&qq,TXT_EZMLM_RESPONSE);
            transferenc();
            copy(&qq,"text/top",flagcd,FATAL);
            copy(&qq,"text/sub-nop",flagcd,FATAL);
            break;
  }
  if (flagdig == FLD_DENY || flagdig == FLD_ALLOW)
    strerr_die3x(0,INFO,ERR_EXTRA_SUB,target.s);
  return r;
}

int getoff(action)
char *action;
{
  int r;

  switch((r = subscribe(workdir,target.s,0,"","-",1,-1,(char *) 0,FATAL))) {
			/* no comment for unsubscribe */
    case 1:
            qmail_puts(&qq,TXT_GOODBYE);
            if (!quote(&quoted,&outlocal)) die_nomem();
            qmail_put(&qq,quoted.s,quoted.len);
            qmail_puts(&qq,"@");
            qmail_put(&qq,outhost.s,outhost.len);
            qmail_puts(&qq,"\n\n");
            transferenc();
            copy(&qq,"text/top",flagcd,FATAL);
            copy(&qq,"text/unsub-ok",flagcd,FATAL);
            break;
    default:
            qmail_puts(&qq,TXT_EZMLM_RESPONSE);
            transferenc();
            copy(&qq,"text/top",flagcd,FATAL);
            copy(&qq,"text/unsub-nop",flagcd,FATAL);
            break;
  }
  if (flagdig == FLD_DENY || flagdig == FLD_ALLOW)
    strerr_die3x(0,INFO,ERR_EXTRA_UNSUB,target.s);
  return r;
}

void doconfirm(act)
/* This should only be called with valid act for sub/unsub confirms. If act */
/* is not ACTION_SC or ACTION_TC, it is assumed to be an unsubscribe conf.*/
char *act;	/* first letter of desired confirm request only as STRING! */
{
  unsigned int i;

  strnum[fmt_ulong(strnum,(unsigned long) when)] = 0;
  cookie(hash,key.s,key.len-flagdig,strnum,target.s,act);
  if (!stralloc_copy(&confirm,&outlocal)) die_nomem();
  if (!stralloc_append(&confirm,"-")) die_nomem();
  if (!stralloc_catb(&confirm,act,1)) die_nomem();
  if (!stralloc_cats(&confirm,"c.")) die_nomem();
  if (!stralloc_cats(&confirm,strnum)) die_nomem();
  if (!stralloc_append(&confirm,".")) die_nomem();
  if (!stralloc_catb(&confirm,hash,COOKIE)) die_nomem();
  if (!stralloc_append(&confirm,"-")) die_nomem();
  if (!stralloc_cats(&confirm,verptarget.s)) die_nomem();
  if (!stralloc_append(&confirm,"@")) die_nomem();
  if (!stralloc_cat(&confirm,&outhost)) die_nomem();
  if (!stralloc_0(&confirm)) die_nomem();
  set_cpconfirm(confirm.s);		/* for copy */

  qmail_puts(&qq,"Reply-To: ");
  if (!quote2(&quoted,confirm.s)) die_nomem();
  qmail_put(&qq,quoted.s,quoted.len);
  qmail_puts(&qq,"\n");
  if (!stralloc_0(&confirm)) die_nomem();

  qmail_puts(&qq,"Subject: ");
  if (*act == ACTION_SC[0] || *act == ACTION_UC[0])
    qmail_puts(&qq,TXT_USRCONFIRM);
  else
    qmail_puts(&qq,TXT_MODCONFIRM);
  if (*act == ACTION_SC[0] || *act == ACTION_TC[0])
    qmail_puts(&qq,TXT_SUBSCRIBE_TO);
  else
    qmail_puts(&qq,TXT_UNSUBSCRIBE_FROM);
  if (!quote(&quoted,&outlocal)) die_nomem();
  qmail_put(&qq,quoted.s,quoted.len);
  qmail_puts(&qq,"@");
  qmail_put(&qq,outhost.s,outhost.len);
  qmail_puts(&qq,"\n");
  transferenc();
    copy(&qq,"text/top",flagcd,FATAL);
}

void sendtomods()
{
  putsubs(moddir.s,0L,52L,subto,1,FATAL);
}

void copybottom()
{
  if (flagbottom || act == AC_HELP) {
    copy(&qq,"text/bottom",flagcd,FATAL);
    if (flagcd) {
      if (flagcd == 'B') {
	encodeB("",0,&line,2,FATAL);	/* flush */
	qmail_put(&qq,line.s,line.len);
      }
      qmail_puts(&qq,"\n--");
      qmail_put(&qq,boundary,COOKIE);
      qmail_puts(&qq,"\nContent-Type: message/rfc822");
      qmail_puts(&qq,"\nContent-Disposition: inline; filename=request.msg\n\n");
    }
    qmail_puts(&qq,"Return-Path: <");
    if (!quote2(&quoted,sender)) die_nomem();
    qmail_put(&qq,quoted.s,quoted.len);
    qmail_puts(&qq,">\n");
    if (seek_begin(0) == -1)
      strerr_die2sys(111,FATAL,ERR_SEEK_INPUT);
    if (substdio_copy(&ssqq,&ssin2) != 0)
      strerr_die2sys(111,FATAL,ERR_READ_INPUT);
    if (flagcd) {
      qmail_puts(&qq,"\n--");
      qmail_put(&qq,boundary,COOKIE);
      qmail_puts(&qq,"--\n");
    }
  } else {
    if (flagcd == 'B') {
      encodeB("",0,&line,2,FATAL);	/* flush even if no bottom */
      qmail_put(&qq,line.s,line.len);
    }
  }

  qmail_from(&qq,from.s);
}

int main(argc,argv)
int argc;
char **argv;
{
  char *local;
  char *def;
  char *action;
  char *x, *y;
  char *fname;
  char *pmod;
  char *err;
  char *cp,*cpfirst,*cplast,*cpnext,*cpafter;
  int flagmod;
  int flagremote;
  int flagpublic;
  int opt,r;
  unsigned int i;
  unsigned int len;
  int fd;
  int flagdone;
  register char ch;

  (void) umask(022);
  sig_pipeignore();
  when = now();

  while ((opt = getopt(argc,argv,"bBcCdDeEfFlLmMnNqQsSuUvV")) != opteof)
    switch(opt) {
      case 'b': flagbottom = 1; break;
      case 'B': flagbottom = 0; break;
      case 'c': flagget = 1; break;
      case 'C': flagget = 0; break;
      case 'd':
      case 'e': flagedit = 1; break;
      case 'D':
      case 'E': flagedit = 0; break;
      case 'f': flagstorefrom = 1; break;
      case 'F': flagstorefrom = 0; break;
      case 'l': flaglist = 1; break;
      case 'L': flaglist = 0; break;
      case 'm': flagunsubismod = 1; break;
      case 'M': flagunsubismod = 0; break;
      case 'n': flagnotify = 1; break;
      case 'N': flagnotify = 0; break;
      case 's': flagsubconf = 1; break;
      case 'S': flagsubconf = 0; break;
      case 'q': flagverbose = 0; break;
      case 'Q': flagverbose++; break;
      case 'u': flagunsubconf = 1; break;
      case 'U': flagunsubconf = 0; break;
      case 'v':
      case 'V': strerr_die2x(0,
		"ezmlm-manage version: ezmlm-0.53+",EZIDX_VERSION);
      default:
	die_usage();
    }

  dir = argv[optind];
  if (!dir) die_usage();

  sender = env_get("SENDER");
  if (!sender) strerr_die2x(100,FATAL,ERR_NOSENDER);
  local = env_get("LOCAL");
  if (!local) strerr_die2x(100,FATAL,ERR_NOLOCAL);
  def = env_get("DEFAULT");

  if (!*sender)
    strerr_die2x(100,FATAL,ERR_BOUNCE);
  if (!sender[str_chr(sender,'@')])
    strerr_die2x(100,FATAL,ERR_ANONYMOUS);
  if (str_equal(sender,"#@[]"))
    strerr_die2x(100,FATAL,ERR_BOUNCE);

  if (chdir(dir) == -1)
    strerr_die4sys(111,FATAL,ERR_SWITCH,dir,": ");

  switch(slurp("key",&key,32)) {
    case -1:
      strerr_die4sys(111,FATAL,ERR_READ,dir,"/key: ");
    case 0:
      strerr_die4x(100,FATAL,dir,"/key",ERR_NOEXIST);
  }
  getconf_line(&mailinglist,"mailinglist",1,FATAL,dir);
  getconf_line(&outhost,"outhost",1,FATAL,dir);
  getconf_line(&outlocal,"outlocal",1,FATAL,dir);
  set_cpouthost(&outhost);
  if (getconf_line(&charset,"charset",0,FATAL,dir)) {
    if (charset.len >= 2 && charset.s[charset.len - 2] == ':') {
      if (charset.s[charset.len - 1] == 'B' ||
		charset.s[charset.len - 1] == 'Q') {
        flagcd = charset.s[charset.len - 1];
        charset.s[charset.len - 2] = '\0';
      }
    }
  } else
    if (!stralloc_copys(&charset,TXT_DEF_CHARSET)) die_nomem();
  if (!stralloc_0(&charset)) die_nomem();

  if (def)			/* qmail-1.02 */
    action = def;		/* .qmail-list-default */
  else {			/* older version of qmail */
    getconf_line(&inlocal,"inlocal",1,FATAL,dir);
    if (inlocal.len > str_len(local)) die_badaddr();
    if (case_diffb(inlocal.s,inlocal.len,local)) die_badaddr();
    action = local + inlocal.len;
    if (*(action++) != '-') die_badaddr();
				/* has to be '-' to match link. Check anyway */
  }

  if (!stralloc_copys(&ddir,dir)) die_nomem();

  if (case_starts(action,"digest")) {			/* digest */
    action += 6;
    if (!stralloc_cats(&outlocal,"-digest")) die_nomem();
    if (!stralloc_cats(&ddir,"/digest")) die_nomem();
    flagdig = FLD_DIGEST;
  } else if (case_starts(action,ACTION_ALLOW)) {	/* allow */
    action += str_len(ACTION_ALLOW);
    if (!stralloc_append(&outlocal,"-")) die_nomem();
    if (!stralloc_cats(&outlocal,ACTION_ALLOW)) die_nomem();
    if (!stralloc_cats(&ddir,"/allow")) die_nomem();
    flagdig = FLD_ALLOW;
  } else if (case_starts(action,ACTION_DENY)) {		/* deny */
    action += str_len(ACTION_DENY);
    if (!stralloc_append(&outlocal,"-")) die_nomem();
    if (!stralloc_cats(&outlocal,ACTION_DENY)) die_nomem();
    if (!stralloc_cats(&ddir,"/deny")) die_nomem();
    flagdig = FLD_DENY;
  }
  if (flagdig)				/* zap '-' after db specifier */
    if (*(action++) != '-') die_badaddr();

  if (!stralloc_0(&ddir)) die_nomem();
  workdir = ddir.s;
  set_cpoutlocal(&outlocal);

  if (!stralloc_copys(&target,sender)) die_nomem();
  if (action[0]) {
    i = str_chr(action,'-');
    if (action[i]) {
      action[i] = 0;
      if (!stralloc_copys(&target,action + i + 1)) die_nomem();
      i = byte_rchr(target.s,target.len,'=');
      if (i < target.len)
	target.s[i] = '@';
    }
  }
  if (!stralloc_0(&target)) die_nomem();
  set_cptarget(target.s);	/* for copy() */
  make_verptarget();

  flagmod = getconf_line(&modsub,"modsub",0,FATAL,dir);
  flagremote = getconf_line(&remote,"remote",0,FATAL,dir);

  if (case_equals(action,ACTION_LISTN) ||
		case_equals(action,ALT_LISTN))
    act = AC_LISTN;
  else if (case_equals(action,ACTION_LIST) ||
		case_equals(action,ALT_LIST))
    act = AC_LIST;
  else if (case_starts(action,ACTION_GET) ||
		case_starts(action,ALT_GET))
    act = AC_GET;
  else if (case_equals(action,ACTION_HELP) ||
		case_equals(action,ALT_HELP))
    act = AC_HELP;
  else if (case_starts(action,ACTION_EDIT) ||
		case_starts(action,ALT_EDIT))
    act = AC_EDIT;
  else if (case_starts(action,ACTION_LOG))
   { act = AC_LOG; actlen = str_len(ACTION_LOG); }
  else if (case_starts(action,ALT_LOG))
   { act = AC_LOG; actlen = str_len(ALT_LOG); }

			/* NOTE: act is needed in msg_headers(). */
			/* Yes, this needs to be cleaned up! */

  if (flagmod || flagremote) {
    if (modsub.len && modsub.s[0] == '/') {
      if (!stralloc_copy(&moddir,&modsub)) die_nomem();
    } else if (remote.len && remote.s[0] == '/') {
      if (!stralloc_copy(&moddir,&remote)) die_nomem();
    } else {
      if (!stralloc_copys(&moddir,dir)) die_nomem();
      if (!stralloc_cats(&moddir,"/mod")) die_nomem();
    }
    if (!stralloc_0(&moddir)) die_nomem();
		/* for these the reply is 'secret' and goes to sender  */
		/* This means that they can be triggered from a SENDER */
		/* that is not a mod, but never send to a non-mod */
    if (act == AC_NONE || flagdig == FLD_DENY)	/* None of the above */
      pmod = issub(moddir.s,sender,(char *) 0,FATAL);
				/* sender = moderator? */
    else
      pmod = issub(moddir.s,target.s,(char *) 0,FATAL);
				/* target = moderator? */
   } else
     pmod = 0;			/* always 0 for non-mod/remote lists */
				/* if DIR/public is missing, we still respond*/
				/* to requests from moderators for remote    */
				/* admin and modsub lists. Since pmod   */
				/* is false for all non-mod lists, only it   */
				/* needs to be tested. */
  if ((flagpublic = slurp("public",&line,1)) == -1)
      strerr_die4sys(111,FATAL,ERR_READ,dir,"/public: ");
  if (!flagpublic && !(pmod && flagremote) &&
                !case_equals(action,ACTION_HELP))
      strerr_die2x(100,FATAL,ERR_NOT_PUBLIC);

  if (flagdig == FLD_DENY)
    if (!pmod || !flagremote)	/* only mods can do */
      strerr_die1x(100,ERR_NOT_ALLOWED);

  if (act == AC_NONE) {		/* none of the above */
    if (case_equals(action,ACTION_SUBSCRIBE) ||
		case_equals(action,ALT_SUBSCRIBE))
      act = AC_SUBSCRIBE;
    else if (case_equals(action,ACTION_UNSUBSCRIBE)
		|| case_equals(action,ALT_UNSUBSCRIBE))
      act = AC_UNSUBSCRIBE;
    else if (str_start(action,ACTION_SC)) act = AC_SC;
  }

  if (!stralloc_copy(&from,&outlocal)) die_nomem();
  if (!stralloc_cats(&from,"-return-@")) die_nomem();
  if (!stralloc_cat(&from,&outhost)) die_nomem();
  if (!stralloc_0(&from)) die_nomem();

  if (qmail_open(&qq,(stralloc *) 0) == -1)
    strerr_die2sys(111,FATAL,ERR_QMAIL_QUEUE);
  msg_headers();

  if (act == AC_SUBSCRIBE) {
    if (pmod && flagremote) {
      doconfirm(ACTION_TC);
      copy(&qq,"text/mod-sub-confirm",flagcd,FATAL);
      copybottom();
      qmail_to(&qq,pmod);
    } else if (flagsubconf) {
      doconfirm(ACTION_SC);
      copy(&qq,"text/sub-confirm",flagcd,FATAL);
      copybottom();
      qmail_to(&qq,target.s);
    } else {				/* normal subscribe, no confirm */
      r = geton(action);		/* should be rarely used. */
      copybottom();
      if (flagnotify) qmail_to(&qq,target.s);
      if (r && flagverbose > 1) to_owner();
    }

  } else if (act == AC_SC) {
    if (hashok(action,ACTION_SC)) {
      if (flagmod && !(pmod && str_equal(sender,target.s))) {
        store_from(&fromline,target.s);	/* save from line, if requested */
					/* since transaction not complete */
        doconfirm(ACTION_TC);
        copy(&qq,"text/mod-sub-confirm",flagcd,FATAL);
        copybottom();
        sendtomods();
      } else {
        r = geton(action);
        copybottom();
        qmail_to(&qq,target.s);
	if (r && flagverbose > 1) to_owner();
      }
    } else {
      doconfirm(ACTION_SC);
      copy(&qq,"text/sub-bad",flagcd,FATAL);
      copybottom();
      qmail_to(&qq,target.s);
    }

  } else if (str_start(action,ACTION_TC)) {
    if (hashok(action,ACTION_TC)) {
      r = geton(action);
      mod_bottom();
      if (flagnotify) qmail_to(&qq,target.s);	/* unless suppressed */
      if (r && flagverbose > 1) to_owner();
    } else {
      if (!pmod || !flagremote)	/* else anyone can get a good -tc. */
        die_cookie();
      doconfirm(ACTION_TC);
      copy(&qq,"text/sub-bad",flagcd,FATAL);
      copybottom();
      qmail_to(&qq,pmod);
    }

  } else if (act == AC_UNSUBSCRIBE) {
    if (flagunsubconf) {
      if (pmod && flagremote) {
        doconfirm(ACTION_VC);
        copy(&qq,"text/mod-unsub-confirm",flagcd,FATAL);
        copybottom();
	qmail_to(&qq,pmod);
      } else {
        doconfirm(ACTION_UC);
        copy(&qq,"text/unsub-confirm",flagcd,FATAL);
        copybottom();
        qmail_to(&qq,target.s);
      }
    } else if (flagunsubismod && flagmod) {
        doconfirm(ACTION_VC);
        copy(&qq,"text/mod-unsub-confirm",flagcd,FATAL);
        copybottom();
        sendtomods();
    } else {
      r = getoff(action);
      copybottom();
      if (!r || flagnotify) qmail_to(&qq,target.s);
		/* tell owner if problems (-Q) or anyway (-QQ) */
      if (flagverbose && (!r || flagverbose > 1)) to_owner();
    }

  } else if (str_start(action,ACTION_UC)) {
    if (hashok(action,ACTION_UC)) {
	/* unsub is moderated only on moderated list if -m unless the */
	/* target == sender == a moderator */
      if (flagunsubismod && flagmod) {
        doconfirm(ACTION_VC);
        copy(&qq,"text/mod-unsub-confirm",flagcd,FATAL);
        copybottom();
        sendtomods();
      } else {
        r = getoff(action);
        copybottom();
        if (!r || flagnotify) qmail_to(&qq,target.s);
		/* tell owner if problems (-Q) or anyway (-QQ) */
	if (flagverbose && (!r || flagverbose > 1)) to_owner();
      }
    } else {
      doconfirm(ACTION_UC);
      copy(&qq,"text/unsub-bad",flagcd,FATAL);
      copybottom();
      qmail_to(&qq,target.s);
    }

  } else if (str_start(action,ACTION_VC)) {
    if (hashok(action,ACTION_VC)) {
      r = getoff(action);
      if (!r && flagmod)
        strerr_die2x(0,INFO,ERR_UNSUB_NOP);
      mod_bottom();
      if (r) {				/* success to target */
	qmail_to(&qq,target.s);
        if (flagverbose > 1) to_owner();
      } else				/* NOP to sender = admin. Will take */
        qmail_to(&qq,sender);		/* care of it. No need to tell owner */
		/* if list is moderated skip - otherwise bad with > 1 mod */
    } else {
      if (!pmod || !flagremote)	/* else anyone can get a good -vc. */
        die_cookie();
      doconfirm(ACTION_VC);
      copy(&qq,"text/unsub-bad",flagcd,FATAL);
      copybottom();
      qmail_to(&qq,pmod);
    }

  } else if (act == AC_LIST || act == AC_LISTN) {

    if (!flaglist || (!flagmod && !flagremote))
      strerr_die2x(100,FATAL,ERR_NOT_AVAILABLE);
    if (!pmod)
      strerr_die2x(100,FATAL,ERR_NOT_ALLOWED);
    qmail_puts(&qq,TXT_EZMLM_RESPONSE);
    transferenc();
    copy(&qq,"text/top",flagcd,FATAL);

    if (act == AC_LIST) {
      (void) code_qput(TXT_LISTMEMBERS,str_len(TXT_LISTMEMBERS));
      i = putsubs(workdir,0L,52L,code_subto,1,FATAL);
    } else			/* listn */
      i = putsubs(workdir,0L,52L,dummy_to,1,FATAL);

    (void) code_qput("\n  ======> ",11);
    (void) code_qput(strnum,fmt_ulong(strnum,i));
    (void) code_qput("\n",1);
    copybottom();
    qmail_to(&qq,pmod);

  } else if (act == AC_LOG) {
    action += actlen;
    if (*action == '.' || *action == '_') ++action;
    if (!flaglist || !flagremote)
      strerr_die2x(100,FATAL,ERR_NOT_AVAILABLE);
    if (!pmod)
      strerr_die2x(100,FATAL,ERR_NOT_ALLOWED);
    qmail_puts(&qq,TXT_EZMLM_RESPONSE);
    transferenc();
    searchlog(workdir,action,code_subto,FATAL);
    copybottom();
    qmail_to(&qq,pmod);

  } else if (act == AC_EDIT) {
	/* only remote admins and only if -e is specified may edit */
    if (!flagedit || !flagremote)
      strerr_die2x(100,FATAL,ERR_NOT_AVAILABLE);
    if (!pmod)
      strerr_die2x(100,FATAL,ERR_NOT_ALLOWED);
    len = str_len(ACTION_EDIT);
    if (!case_starts(action,ACTION_EDIT))
      len = str_len(ALT_EDIT);
    if (action[len]) {			/* -edit.file, not just -edit */
      if (action[len] != '.')
        strerr_die2x(100,FATAL,ERR_BAD_REQUEST);
      if (!stralloc_copys(&fnedit,"text/")) die_nomem();
      if (!stralloc_cats(&fnedit,action+len+1)) die_nomem();
      if (!stralloc_0(&fnedit)) die_nomem();
      case_lowerb(fnedit.s,fnedit.len);
      i = 5;	/* after the "text/" */
      while ((ch = fnedit.s[i++])) {
        if (((ch > 'z') || (ch < 'a')) && (ch != '_'))
          strerr_die2x(100,FATAL,ERR_BAD_NAME);
        if (ch == '_') fnedit.s[i-1] = '-';
      }
      switch(slurp(fnedit.s,&text,1024)) {	/* entire file! */
        case -1:
          strerr_die6sys(111,FATAL,ERR_READ,dir,"/",fnedit.s,": ");
        case 0:
          strerr_die5x(100,FATAL,dir,"/",fnedit.s,ERR_NOEXIST);
      }
      if (!stralloc_copy(&line,&text)) die_nomem();
      {		/* get rid of nulls to use cookie */
        register char *s; register unsigned int n;
        s = line.s; n = line.len;
        while(n--) { if (!*s) *s = '_'; ++s; }
      }
      if (!stralloc_cat(&line,&fnedit)) die_nomem();	/* including '\0' */
      strnum[fmt_ulong(strnum,(unsigned long) when)] = 0;
      cookie(hash,key.s,key.len,strnum,line.s,"-e");
      if (!stralloc_copy(&confirm,&outlocal)) die_nomem();
      if (!stralloc_append(&confirm,"-")) die_nomem();
      if (!stralloc_catb(&confirm,ACTION_ED,LENGTH_ED)) die_nomem();
      if (!stralloc_cats(&confirm,strnum)) die_nomem();
      if (!stralloc_append(&confirm,".")) die_nomem();
		/* action part has been checked for bad chars */
      if (!stralloc_cats(&confirm,action + len + 1)) die_nomem();
      if (!stralloc_append(&confirm,".")) die_nomem();
      if (!stralloc_catb(&confirm,hash,COOKIE)) die_nomem();
      if (!stralloc_append(&confirm,"@")) die_nomem();
      if (!stralloc_cat(&confirm,&outhost)) die_nomem();
      if (!stralloc_0(&confirm)) die_nomem();
      set_cpconfirm(confirm.s);

      qmail_puts(&qq,"Reply-To: ");
      if (!quote2(&quoted,confirm.s)) die_nomem();
      qmail_put(&qq,quoted.s,quoted.len);
      qmail_puts(&qq,"\n");
      if (!stralloc_0(&confirm)) die_nomem();

      qmail_puts(&qq,TXT_EDIT_RESPONSE);
      qmail_puts(&qq,action+len+1);	/* has the '_' not '-' */
      qmail_puts(&qq,TXT_EDIT_FOR);
      if (!quote(&quoted,&outlocal)) die_nomem();
      qmail_put(&qq,quoted.s,quoted.len);
      qmail_puts(&qq,"@");
      qmail_put(&qq,outhost.s,outhost.len);
      qmail_puts(&qq,"\n");
      transferenc();
      copy(&qq,"text/top",flagcd,FATAL);
      copy(&qq,"text/edit-do",flagcd,FATAL);
      (void) code_qput(TXT_EDIT_START,str_len(TXT_EDIT_START));
      (void) code_qput("\n",1);
      (void) code_qput(text.s,text.len);
      (void) code_qput(TXT_EDIT_END,str_len(TXT_EDIT_END));
      (void) code_qput("\n",1);

    } else {	/* -edit only, so output list of editable files */
      qmail_puts(&qq,TXT_EDIT_LIST);
      transferenc();
      copy(&qq,"text/top",flagcd,FATAL);
      copy(&qq,"text/edit-list",flagcd,FATAL);
    }
    qmail_puts(&qq,"\n\n");
    copybottom();
    qmail_to(&qq,pmod);

  } else if (str_start(action,ACTION_ED)) {
    datetime_sec u;
    int flaggoodfield;
    x = action + LENGTH_ED;
    x += scan_ulong(x,&u);
    if ((u > when) || (u < when - 100000)) die_cookie();
    if (*x == '.') ++x;
    fname = x;
    x += str_chr(x,'.');
    if (!*x) die_cookie();
    *x = (char) 0;
    ++x;
    if (!stralloc_copys(&fnedit,"text/")) die_nomem();
    if (!stralloc_cats(&fnedit,fname)) die_nomem();
    if (!stralloc_0(&fnedit)) die_nomem();
    y = fnedit.s + 5;		/* after "text/" */
    while (*++y) {		/* Name should be guaranteed by the cookie, */
				/* but better safe than sorry ... */
      if (((*y > 'z') || (*y < 'a')) && (*y != '_'))
          strerr_die2x(100,FATAL,ERR_BAD_NAME);
      if (*y == '_') *y = '-';
    }

    lock();			/* file must not change while here */

    switch (slurp(fnedit.s,&text,1024)) {
      case -1:
        strerr_die6sys(111,FATAL,ERR_READ,dir,"/",fnedit.s,": ");
      case 0:
        strerr_die5x(100,FATAL,dir,"/",fnedit.s,ERR_NOEXIST);
    }
    if (!stralloc_copy(&line,&text)) die_nomem();
    {		/* get rid of nulls to use cookie */
      register char *s; register unsigned int n;
      s = line.s; n = line.len;
      while(n--) { if (!*s) *s = '_'; ++s; }
    }
    if (!stralloc_cat(&line,&fnedit)) die_nomem();	/* including '\0' */
    strnum[fmt_ulong(strnum,(unsigned long) u)] = 0;
    cookie(hash,key.s,key.len,strnum,line.s,"-e");
    if (str_len(x) != COOKIE) die_cookie();
    if (byte_diff(hash,COOKIE,x)) die_cookie();
	/* cookie is ok, file exists, lock's on, new file ends in '_' */
    if (!stralloc_copys(&fneditn,fnedit.s)) die_nomem();
    if (!stralloc_append(&fneditn,"_")) die_nomem();
    if (!stralloc_0(&fneditn)) die_nomem();
    fd = open_trunc(fneditn.s);
    if (fd == -1)
      strerr_die6sys(111,FATAL,ERR_WRITE,dir,"/",fneditn.s,": ");
    substdio_fdbuf(&sstext,write,fd,textbuf,sizeof(textbuf));
    if (!stralloc_copys(&quoted,"")) die_nomem();	/* clear */
    if (!stralloc_copys(&text,"")) die_nomem();

    for (;;) {			/* get message body */
      if (getln(&ssin,&line,&match,'\n') == -1)
        strerr_die2sys(111,FATAL,ERR_READ_INPUT);
      if (!match) break;
      if (!stralloc_cat(&text,&line)) die_nomem();
    }
    if (encin) {	/* decode if necessary */
      if (encin == 'B')
        decodeB(text.s,text.len,&line,FATAL);
      else
        decodeQ(text.s,text.len,&line,FATAL);
      if (!stralloc_copy(&text,&line)) die_nomem();
    }
    cp = text.s;
    cpafter = text.s+text.len;
    flaggoodfield = 0;
    flagdone = 0;
    len = 0;
    while ((cpnext = cp + byte_chr(cp,cpafter-cp,'\n')) != cpafter) {
      i = byte_chr(cp,cpnext-cp,'%');
      if (i != (unsigned int) (cpnext - cp)) {
        if (!flaggoodfield) {	/* TXT_EDIT_START/END */
          if (case_startb(cp+i,cpnext-cp-i,TXT_EDIT_START)) {
		/* start tag. Store users 'quote characters', e.g. '> ' */
            if (!stralloc_copyb(&quoted,cp,i)) die_nomem();
            flaggoodfield = 1;
            cp = cpnext + 1;
            cpfirst = cp;
            continue;
          }
        } else
          if (case_startb(cp+i,cpnext-cp-i,TXT_EDIT_END)) {
            flagdone = 1;
            break;
          }
      }
      if (flaggoodfield) {
        if ((len += cpnext - cp - quoted.len + 1) > MAXEDIT)
          strerr_die1x(100,ERR_EDSIZE);

        if (quoted.len && cpnext-cp >= (int) quoted.len &&
			!str_diffn(cp,quoted.s,quoted.len))
          cp += quoted.len;	/* skip quoting characters */
        cplast = cpnext - 1;
        if (*cplast == '\r')	/* CRLF -> '\n' for base64 encoding */
          *cplast = '\n';
        else
          ++cplast;
        if (substdio_put(&sstext,cp,cplast-cp+1) == -1)
            strerr_die6sys(111,FATAL,ERR_WRITE,dir,"/",fneditn.s,": ");
      }
      cp = cpnext + 1;
    }
    if (!flagdone)
      strerr_die2x(100,FATAL,ERR_NO_MARK);
    if (substdio_flush(&sstext) == -1)
      strerr_die6sys(111,FATAL,ERR_WRITE,dir,"/",fneditn.s,": ");
    if (fsync(fd) == -1)
      strerr_die6sys(111,FATAL,ERR_SYNC,dir,"/",fneditn.s,": ");
    if (fchmod(fd, 0600) == -1)
      strerr_die6sys(111,FATAL,ERR_CHMOD,dir,"/",fneditn.s,": ");
    if (close(fd) == -1)
      strerr_die6sys(111,FATAL,ERR_CLOSE,dir,"/",fneditn.s,": ");
    if (rename(fneditn.s,fnedit.s) == -1)
      strerr_die6sys(111,FATAL,ERR_MOVE,dir,"/",fneditn.s,": ");

    unlock();
    qmail_puts(&qq,TXT_EDIT_SUCCESS);
    qmail_puts(&qq,fname);
    qmail_puts(&qq,TXT_EDIT_FOR);
    if (!quote(&quoted,&outlocal)) die_nomem();
    qmail_put(&qq,quoted.s,quoted.len);
    qmail_puts(&qq,"@");
    qmail_put(&qq,outhost.s,outhost.len);
    qmail_puts(&qq,"\n");
    transferenc();
    copy(&qq,"text/top",flagcd,FATAL);
    copy(&qq,"text/edit-done",flagcd,FATAL);
    copybottom();
    qmail_to(&qq,sender);	/* not necessarily from mod */

  } else if (act == AC_GET) {

    unsigned long u;
    struct stat st;
    char ch;
    int r;
    unsigned int pos;

    if (!flagget)
      strerr_die2x(100,FATAL,ERR_NOT_AVAILABLE);
    qmail_puts(&qq,TXT_EZMLM_RESPONSE);
    transferenc();
    copy(&qq,"text/top",flagcd,FATAL);

    pos = str_len(ACTION_GET);
    if (!case_starts(action,ACTION_GET))
      pos = str_len(ALT_GET);

    if (action[pos] == '.' || action [pos] == '_') pos++;
    scan_ulong(action + pos,&u);

    if (!stralloc_copys(&line,"archive/")) die_nomem();
    if (!stralloc_catb(&line,strnum,fmt_ulong(strnum,u / 100))) die_nomem();
    if (!stralloc_cats(&line,"/")) die_nomem();
    if (!stralloc_catb(&line,strnum,fmt_uint0(strnum,(unsigned int) (u % 100),2))) die_nomem();
    if (!stralloc_0(&line)) die_nomem();

    fd = open_read(line.s);
    if (fd == -1)
      if (errno != error_noent)
	strerr_die4sys(111,FATAL,ERR_OPEN,line.s,": ");
      else
        copy(&qq,"text/get-bad",flagcd,FATAL);
    else {
      if (fstat(fd,&st) == -1)
        copy(&qq,"text/get-bad",flagcd,FATAL);
      else if (!(st.st_mode & 0100))
        copy(&qq,"text/get-bad",flagcd,FATAL);
      else {
        substdio_fdbuf(&sstext,read,fd,textbuf,sizeof(textbuf));
	qmail_puts(&qq,"> ");
	for (;;) {
	  r = substdio_get(&sstext,&ch,1);
	  if (r == -1) strerr_die4sys(111,FATAL,ERR_READ,line.s,": ");
	  if (r == 0) break;
	  qmail_put(&qq,&ch,1);
	  if (ch == '\n') qmail_puts(&qq,"> ");
	}
	qmail_puts(&qq,"\n");
      }
      close(fd);
    }
    copybottom();
    qmail_to(&qq,target.s);

  } else if (case_starts(action,ACTION_QUERY) ||
		case_starts(action,ALT_QUERY)) {
    qmail_puts(&qq,TXT_EZMLM_RESPONSE);
    transferenc();
    copy(&qq,"text/top",flagcd,FATAL);
    if (pmod) {	/* pmod points to static storage in issub(). Need to do this */
		/* before calling issub() again */
      if (!stralloc_copys(&to,pmod)) die_nomem();
      if (!stralloc_0(&to)) die_nomem();
    } else {
      if (!stralloc_copy(&to,&target)) die_nomem();
    }
    if (issub(workdir,target.s,(char *) 0,FATAL))
      copy(&qq,"text/sub-nop",flagcd,FATAL);
    else
      copy(&qq,"text/unsub-nop",flagcd,FATAL);
    copybottom();
    qmail_to(&qq,to.s);

  } else if (case_starts(action,ACTION_INFO) ||
		case_starts(action,ALT_INFO)) {
    qmail_puts(&qq,TXT_EZMLM_RESPONSE);
    transferenc();
    copy(&qq,"text/top",flagcd,FATAL);
    copy(&qq,"text/info",flagcd,FATAL);
    copybottom();
    qmail_to(&qq,target.s);

  } else if (case_starts(action,ACTION_FAQ) ||
		case_starts(action,ALT_FAQ)) {
    qmail_puts(&qq,TXT_EZMLM_RESPONSE);
    transferenc();
    copy(&qq,"text/top",flagcd,FATAL);
    copy(&qq,"text/faq",flagcd,FATAL);
    copybottom();
    qmail_to(&qq,target.s);

  } else if (pmod && (act == AC_HELP)) {
    qmail_puts(&qq,TXT_EZMLM_RESPONSE);
    transferenc();
    copy(&qq,"text/top",flagcd,FATAL);
    copy(&qq,"text/mod-help",flagcd,FATAL);
    copy(&qq,"text/help",flagcd,FATAL);
    copybottom();
    qmail_to(&qq,pmod);

  } else {
    act = AC_HELP;
    qmail_puts(&qq,TXT_EZMLM_RESPONSE);
    transferenc();
    copy(&qq,"text/top",flagcd,FATAL);
    copy(&qq,"text/help",flagcd,FATAL);
    copybottom();
    qmail_to(&qq,sender);
  }

  if (*(err = qmail_close(&qq)) == '\0') {
      strnum[fmt_ulong(strnum,qmail_qp(&qq))] = 0;
      closesql();
      strerr_die2x(0,"ezmlm-manage: info: qp ",strnum);
  } else {
      closesql();
      strerr_die3x(111,FATAL,ERR_TMP_QMAIL_QUEUE,err + 1);
  }
}
