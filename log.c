#include "substdio.h"
#include "readwrite.h"
#include "stralloc.h"
#include "log.h"
#include "now.h"
#include "fmt.h"
#include "open.h"

/* appends (not crash-proof) a line to "Log". The format is: */
/* "timestamp event address[ comment]\n". address is free of ' ' */
/* Unprintable chars are changed to '?'. Comment may have spaces */

static substdio ss;
static char buf[1];
static char num[FMT_ULONG];
static stralloc line = {0};
static stralloc fn = {0};

void log(dir,event,addr,comment)
char *dir;
char *event;
char *addr;
char *comment;
{
  char ch;
  int fd;

  if (!stralloc_copyb(&line,num,fmt_ulong(num,(unsigned long) now()))) return;
  if (!stralloc_cats(&line," ")) return;
  if (!stralloc_cats(&line,event)) return;
  if (!stralloc_cats(&line," ")) return;
  while (ch = *addr++) {
    if ((ch < 33) || (ch > 126)) ch = '?';
    if (!stralloc_append(&line,&ch)) return;
  }
  if (comment && *comment) {
    if (!stralloc_cats(&line," ")) return;
    while (ch = *comment++) {
      if (ch == '\t')
        ch = ' ';
      else 
        if ((ch < 32) || (ch > 126)) ch = '?';
      if (!stralloc_append(&line,&ch)) return;
    }
  }
  if (!stralloc_cats(&line,"\n")) return;

  if (!stralloc_copys(&fn,dir)) return;
  if (!stralloc_cats(&fn,"/Log")) return;
  if (!stralloc_0(&fn)) return;
  fd = open_append(fn.s);
  if (fd == -1) return;
  substdio_fdbuf(&ss,write,fd,buf,sizeof(buf));
  substdio_putflush(&ss,line.s,line.len);
  close(fd);
  return;
}