/*$Id: issub.c,v 1.16 1999/12/11 03:04:19 lindberg Exp $*/
/*$Name: ezmlm-idx-040 $*/
#include "stralloc.h"
#include "getln.h"
#include "readwrite.h"
#include "substdio.h"
#include "open.h"
#include "byte.h"
#include "case.h"
#include "strerr.h"
#include "error.h"
#include "uint32.h"
#include "fmt.h"
#include "subscribe.h"
#include "errtxt.h"
#include <mysql.h>

static void die_nomem(fatal)
char *fatal;
{
  strerr_die2x(111,fatal,ERR_NOMEM);
}

static stralloc addr = {0};
static stralloc lcaddr = {0};
static stralloc line = {0};
static stralloc quoted = {0};
static stralloc fn = {0};
static substdio ss;
static char ssbuf[512];
static char szh[FMT_ULONG];

char *issub(dbname,userhost,tab,fatal)
/* Returns (char *) to match if userhost is in the subscriber database     */
/* dbname, 0 otherwise. dbname is a base directory for a list and may NOT  */
/* be NULL        */
/* NOTE: The returned pointer is NOT VALID after a subsequent call to issub!*/

char *dbname;		/* directory to basedir */
char *userhost;
char *tab;		/* override table name */
char *fatal;

{
  MYSQL_RES *result;
  MYSQL_ROW row;
  char *ret;
  char *table;
  unsigned long *lengths;

  int fd;
  unsigned int j;
  uint32 h,lch;
  char ch,lcch;
  int match;

  table = tab;
  if ((ret = opensql(dbname,&table))) {
    if (*ret) strerr_die2x(111,fatal,ret);
						/* fallback to local db */

    if (!stralloc_copys(&addr,"T")) die_nomem(fatal);
    if (!stralloc_cats(&addr,userhost)) die_nomem(fatal);

    j = byte_rchr(addr.s,addr.len,'@');
    if (j == addr.len) return 0;
    case_lowerb(addr.s + j + 1,addr.len - j - 1);
    if (!stralloc_copy(&lcaddr,&addr)) die_nomem(fatal);
    case_lowerb(lcaddr.s + 1,j - 1);	/* totally lc version of addr */

    h = 5381;
    lch = h;			/* make hash for both for backwards comp */
    for (j = 0;j < addr.len;++j) {	/* (lcaddr.len == addr.len) */
      h = (h + (h << 5)) ^ (uint32) (unsigned char) addr.s[j];
      lch = (lch + (lch << 5)) ^ (uint32) (unsigned char) lcaddr.s[j];
    }
    ch = 64 + (h % 53);
    lcch = 64 + (lch % 53);

    if (!stralloc_0(&addr)) die_nomem(fatal);
    if (!stralloc_0(&lcaddr)) die_nomem(fatal);
    if (!stralloc_copys(&fn,dbname)) die_nomem(fatal);
    if (!stralloc_cats(&fn,"/subscribers/")) die_nomem(fatal);
    if (!stralloc_catb(&fn,&lcch,1)) die_nomem(fatal);
    if (!stralloc_0(&fn)) die_nomem(fatal);

    fd = open_read(fn.s);
    if (fd == -1) {
      if (errno != error_noent)
        strerr_die4sys(111,fatal,ERR_OPEN,fn.s,": ");
    } else {
      substdio_fdbuf(&ss,read,fd,ssbuf,sizeof(ssbuf));

      for (;;) {
        if (getln(&ss,&line,&match,'\0') == -1)
          strerr_die4sys(111,fatal,ERR_READ,fn.s,": ");
        if (!match) break;
        if (line.len == lcaddr.len)
          if (!case_diffb(line.s,line.len,lcaddr.s))
            { close(fd); return line.s+1; }
      }

      close(fd);
    }
	/* here if file not found or (file found && addr not there) */

    if (ch == lcch) return 0;

	/* try case sensitive hash for backwards compatibility */
    fn.s[fn.len - 2] = ch;
    fd = open_read(fn.s);
    if (fd == -1) {
      if (errno != error_noent)
        strerr_die4sys(111,fatal,ERR_OPEN,fn.s,": ");
      return 0;
    }
    substdio_fdbuf(&ss,read,fd,ssbuf,sizeof(ssbuf));

    for (;;) {
      if (getln(&ss,&line,&match,'\0') == -1)
        strerr_die4sys(111,fatal,ERR_READ,fn.s,": ");
      if (!match) break;
      if (line.len == addr.len)
        if (!case_diffb(line.s,line.len,addr.s))
          { close(fd); return line.s+1; }
    }

    close(fd);

    return 0;
  } else {						/* SQL version  */
	/* SELECT address FROM list WHERE address = 'userhost' AND hash */
	/* BETWEEN 0 AND 52. Without the hash restriction, we'd make it */
	/* even easier to defeat. Just faking sender to the list name would*/
	/* work. Since sender checks for posts are bogus anyway, I don't */
	/* know if it's worth the cost of the "WHERE ...". */

    if (!stralloc_copys(&addr,userhost)) die_nomem(fatal);
    j = byte_rchr(addr.s,addr.len,'@');
    if (j == addr.len) return 0;
    case_lowerb(addr.s + j + 1,addr.len - j - 1);

    if (!stralloc_copys(&line,"SELECT address FROM ")) die_nomem(fatal);
    if (!stralloc_cats(&line,table)) die_nomem(fatal);
    if (!stralloc_cats(&line," WHERE address = '")) die_nomem(fatal);
    if (!stralloc_ready(&quoted,2 * addr.len + 1)) die_nomem(fatal);
    if (!stralloc_catb(&line,quoted.s,
	mysql_escape_string(quoted.s,userhost,addr.len))) die_nomem(fatal);
    if (!stralloc_cats(&line,"'"))
		die_nomem(fatal);
    if (mysql_real_query((MYSQL *) psql,line.s,line.len))	/* query */
		strerr_die2x(111,fatal,mysql_error((MYSQL *) psql));
    if (!(result = mysql_use_result((MYSQL *) psql)))
		strerr_die2x(111,fatal,mysql_error((MYSQL *) psql));
    row = mysql_fetch_row(result);
    ret = (char *) 0;
    if (!row) {		/* we need to return the actual address as other */
			/* dbs may accept user-*@host, but we still want */
			/* to make sure to send to e.g the correct moderator*/
			/* address. */
      if (!mysql_eof(result))
		strerr_die2x(111,fatal,mysql_error((MYSQL *) psql));
    } else {
      if (!(lengths = mysql_fetch_lengths(result)))
		strerr_die2x(111,fatal,mysql_error((MYSQL *) psql));
      if (!stralloc_copyb(&line,row[0],lengths[0])) die_nomem(fatal);
      if (!stralloc_0(&line)) die_nomem(fatal);
      ret = line.s;
      while ((row = mysql_fetch_row(result)));	/* maybe not necessary */
      mysql_free_result(result);
    }
    return ret;
  }
}