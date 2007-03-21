/*
 * Copyright (C) 1996-2000 Michael R. Elkins <me@mutt.org>
 * Copyright (C) 1999-2000 Thomas Roessler <roessler@does-not-exist.org>
 * 
 *     This program is free software; you can redistribute it
 *     and/or modify it under the terms of the GNU General Public
 *     License as published by the Free Software Foundation; either
 *     version 2 of the License, or (at your option) any later
 *     version.
 * 
 *     This program is distributed in the hope that it will be
 *     useful, but WITHOUT ANY WARRANTY; without even the implied
 *     warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *     PURPOSE.  See the GNU General Public License for more
 *     details.
 * 
 *     You should have received a copy of the GNU General Public
 *     License along with this program; if not, write to the Free
 *     Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *     Boston, MA  02110-1301, USA.
 */ 

/*
 * This file used to contain some more functions, namely those
 * which are now in muttlib.c.  They have been removed, so we have
 * some of our "standard" functions in external programs, too.
 */

#define _LIB_C 1

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pwd.h>

#ifdef HAVE_SYSEXITS_H
#include <sysexits.h>
#else /* Make sure EX_OK is defined <philiph@pobox.com> */
#define EX_OK 0
#endif

#include "lib.h"


static struct sysexits
{
  int v;
  const char *str;
} 
sysexits_h[] = 
{
#ifdef EX_USAGE
  { 0xff & EX_USAGE, "Bad usage." },
#endif
#ifdef EX_DATAERR
  { 0xff & EX_DATAERR, "Data format error." },
#endif
#ifdef EX_NOINPUT
  { 0xff & EX_NOINPUT, "Cannot open input." },
#endif
#ifdef EX_NOUSER
  { 0xff & EX_NOUSER, "User unknown." },
#endif
#ifdef EX_NOHOST
  { 0xff & EX_NOHOST, "Host unknown." },
#endif
#ifdef EX_UNAVAILABLE
  { 0xff & EX_UNAVAILABLE, "Service unavailable." },
#endif
#ifdef EX_SOFTWARE
  { 0xff & EX_SOFTWARE, "Internal error." },
#endif
#ifdef EX_OSERR
  { 0xff & EX_OSERR, "Operating system error." },
#endif
#ifdef EX_OSFILE
  { 0xff & EX_OSFILE, "System file missing." },
#endif
#ifdef EX_CANTCREAT
  { 0xff & EX_CANTCREAT, "Can't create output." },
#endif
#ifdef EX_IOERR
  { 0xff & EX_IOERR, "I/O error." },
#endif
#ifdef EX_TEMPFAIL
  { 0xff & EX_TEMPFAIL, "Deferred." },
#endif
#ifdef EX_PROTOCOL
  { 0xff & EX_PROTOCOL, "Remote protocol error." },
#endif
#ifdef EX_NOPERM
  { 0xff & EX_NOPERM, "Insufficient permission." },
#endif
#ifdef EX_CONFIG
  { 0xff & EX_NOPERM, "Local configuration error." },
#endif
  { S_ERR, "Exec error." },
  { -1, NULL}
};

void mutt_nocurses_error (const char *fmt, ...)
{
  va_list ap;

  va_start (ap, fmt);
  vfprintf (stderr, fmt, ap);
  va_end (ap);
  fputc ('\n', stderr);
}

void *safe_calloc (size_t nmemb, size_t size)
{
  void *p;

  if (!nmemb || !size)
    return NULL;

  if (((size_t) -1) / nmemb <= size)
  {
    mutt_error _("Integer overflow -- can't allocate memory!");
    sleep (1);
    mutt_exit (1);
  }
  
  if (!(p = calloc (nmemb, size)))
  {
    mutt_error _("Out of memory!");
    sleep (1);
    mutt_exit (1);
  }
  return p;
}

void *safe_malloc (size_t siz)
{
  void *p;

  if (siz == 0)
    return 0;
  if ((p = (void *) malloc (siz)) == 0)	/* __MEM_CHECKED__ */
  {
    mutt_error _("Out of memory!");
    sleep (1);
    mutt_exit (1);
  }
  return (p);
}

void safe_realloc (void *ptr, size_t siz)
{
  void *r;
  void **p = (void **)ptr;

  if (siz == 0)
  {
    if (*p)
    {
      free (*p);			/* __MEM_CHECKED__ */
      *p = NULL;
    }
    return;
  }

  if (*p)
    r = (void *) realloc (*p, siz);	/* __MEM_CHECKED__ */
  else
  {
    /* realloc(NULL, nbytes) doesn't seem to work under SunOS 4.1.x  --- __MEM_CHECKED__ */
    r = (void *) malloc (siz);		/* __MEM_CHECKED__ */
  }

  if (!r)
  {
    mutt_error _("Out of memory!");
    sleep (1);
    mutt_exit (1);
  }

  *p = r;
}

void safe_free (void *ptr)	/* __SAFE_FREE_CHECKED__ */
{
  void **p = (void **)ptr;
  if (*p)
  {
    free (*p);				/* __MEM_CHECKED__ */
    *p = 0;
  }
}

int safe_fclose (FILE **f)
{
  int r = 0;
  
  if (*f)
    r = fclose (*f);
      
  *f = NULL;
  return r;
}

char *safe_strdup (const char *s)
{
  char *p;
  size_t l;

  if (!s || !*s)
    return 0;
  l = strlen (s) + 1;
  p = (char *)safe_malloc (l);
  memcpy (p, s, l);
  return (p);
}

char *safe_strcat (char *d, size_t l, const char *s)
{
  char *p = d;

  if (!l) 
    return d;

  l--; /* Space for the trailing '\0'. */
  
  for (; *d && l; l--)
    d++;
  for (; *s && l; l--)
    *d++ = *s++;

  *d = '\0';
  
  return p;
}

char *safe_strncat (char *d, size_t l, const char *s, size_t sl)
{
  char *p = d;

  if (!l)
    return d;
  
  l--; /* Space for the trailing '\0'. */
  
  for (; *d && l; l--)
    d++;
  for (; *s && l && sl; l--, sl--)
    *d++ = *s++;

  *d = '\0';
  
  return p;
}


void mutt_str_replace (char **p, const char *s)
{
  FREE (p);		/* __FREE_CHECKED__ */
  *p = safe_strdup (s);
}

void mutt_str_adjust (char **p)
{
  if (!p || !*p) return;
  safe_realloc (p, strlen (*p) + 1);
}

/* convert all characters in the string to lowercase */
char *mutt_strlower (char *s)
{
  char *p = s;

  while (*p)
  {
    *p = tolower ((unsigned char) *p);
    p++;
  }

  return (s);
}

void mutt_unlink (const char *s)
{
  int fd;
  int flags;
  FILE *f;
  struct stat sb, sb2;
  char buf[2048];

  /* Defend against symlink attacks */
  
#ifdef O_NOFOLLOW 
  flags = O_RDWR | O_NOFOLLOW;
#else
  flags = O_RDWR;
#endif
  
  if (lstat (s, &sb) == 0 && S_ISREG(sb.st_mode))
  {
    if ((fd = open (s, flags)) < 0)
      return;
    
    if ((fstat (fd, &sb2) != 0) || !S_ISREG (sb2.st_mode) 
	|| (sb.st_dev != sb2.st_dev) || (sb.st_ino != sb2.st_ino))
    {
      close (fd);
      return;
    }
    
    if ((f = fdopen (fd, "r+")))
    {
      unlink (s);
      memset (buf, 0, sizeof (buf));
      while (sb.st_size > 0)
      {
	fwrite (buf, 1, MIN (sizeof (buf), sb.st_size), f);
	sb.st_size -= MIN (sizeof (buf), sb.st_size);
      }
      fclose (f);
    }
  }
}

int mutt_copy_bytes (FILE *in, FILE *out, size_t size)
{
  char buf[2048];
  size_t chunk;

  while (size > 0)
  {
    chunk = (size > sizeof (buf)) ? sizeof (buf) : size;
    if ((chunk = fread (buf, 1, chunk, in)) < 1)
      break;
    if (fwrite (buf, 1, chunk, out) != chunk)
    {
      /* dprint (1, (debugfile, "mutt_copy_bytes(): fwrite() returned short byte count\n")); */
      return (-1);
    }
    size -= chunk;
  }

  return 0;
}

int mutt_copy_stream (FILE *fin, FILE *fout)
{
  size_t l;
  char buf[LONG_STRING];

  while ((l = fread (buf, 1, sizeof (buf), fin)) > 0)
  {
    if (fwrite (buf, 1, l, fout) != l)
      return (-1);
  }

  return 0;
}

int 
compare_stat (struct stat *osb, struct stat *nsb)
{
  if (osb->st_dev != nsb->st_dev || osb->st_ino != nsb->st_ino ||
      osb->st_rdev != nsb->st_rdev)
  {
    return -1;
  }

  return 0;
}

int safe_symlink(const char *oldpath, const char *newpath)
{
  struct stat osb, nsb;

  if(!oldpath || !newpath)
    return -1;
  
  if(unlink(newpath) == -1 && errno != ENOENT)
    return -1;
  
  if (oldpath[0] == '/')
  {
    if (symlink (oldpath, newpath) == -1)
      return -1;
  }
  else
  {
    char abs_oldpath[_POSIX_PATH_MAX];

    if ((getcwd (abs_oldpath, sizeof abs_oldpath) == NULL) ||
	(strlen (abs_oldpath) + 1 + strlen (oldpath) + 1 > sizeof abs_oldpath))
    return -1;
  
    strcat (abs_oldpath, "/");		/* __STRCAT_CHECKED__ */
    strcat (abs_oldpath, oldpath);	/* __STRCAT_CHECKED__ */
    if (symlink (abs_oldpath, newpath) == -1)
      return -1;
  }

  if(stat(oldpath, &osb) == -1 || stat(newpath, &nsb) == -1
     || compare_stat(&osb, &nsb) == -1)
  {
    unlink(newpath);
    return -1;
  }
  
  return 0;
}



/* 
 * This function is supposed to do nfs-safe renaming of files.
 * 
 * Warning: We don't check whether src and target are equal.
 */

int safe_rename (const char *src, const char *target)
{
  struct stat ssb, tsb;

  if (!src || !target)
    return -1;

  if (link (src, target) != 0)
  {

    /*
     * Coda does not allow cross-directory links, but tells
     * us it's a cross-filesystem linking attempt.
     * 
     * However, the Coda rename call is allegedly safe to use.
     * 
     * With other file systems, rename should just fail when 
     * the files reside on different file systems, so it's safe
     * to try it here.
     *
     */
    
    dprint (1, (debugfile, "safe_rename: link (%s, %s) failed: %s (%d)\n", src, target, strerror (errno), errno));

    if (errno == EXDEV)
    {
      dprint (1, (debugfile, "safe_rename: errno was EXDEV; trying rename...\n"));
      if (rename (src, target) == -1) 
      {
	dprint (1, (debugfile, "safe_rename: rename (%s, %s) failed: %s (%d)\n", src, target, strerror (errno), errno));
	return -1;
      }
      dprint (1, (debugfile, "safe_rename: rename succeeded.\n"));
    
      return 0;
    }

    return -1;
  }

  /*
   * Stat both links and check if they are equal.
   */
  
  if (lstat (src, &ssb) == -1)
  {
    dprint (1, (debugfile, "safe_rename: can't stat %s: %s (%d)\n",
		src, strerror (errno), errno));
    return -1;
  }
  
  if (lstat (target, &tsb) == -1)
  {
    dprint (1, (debugfile, "safe_rename: can't stat %s: %s (%d)\n",
		src, strerror (errno), errno));
    return -1;
  }

  /* 
   * pretend that the link failed because the target file
   * did already exist.
   */

  if (compare_stat (&ssb, &tsb) == -1)
  {
    dprint (1, (debugfile, "safe_rename: stat blocks for %s and %s diverge; pretending EEXIST.\n", src, target));
    errno = EEXIST;
    return -1;
  }

  /*
   * Unlink the original link.  Should we really ignore the return
   * value here? XXX
   */

  if (unlink (src) == -1) 
  {
    dprint (1, (debugfile, "safe_rename: unlink (%s) failed: %s (%d)\n",
		src, strerror (errno), errno));
  }
  

  return 0;
}


/* Create a temporary directory next to a file name */

int mutt_mkwrapdir (const char *path, char *newfile, size_t nflen, 
		    char *newdir, size_t ndlen)
{
  const char *basename;
  char parent[_POSIX_PATH_MAX];
  char *p;
  int rv;

  strfcpy (parent, NONULL (path), sizeof (parent));
  
  if ((p = strrchr (parent, '/')))
  {
    *p = '\0';
    basename = p + 1;
  }
  else
  {
    strfcpy (parent, ".", sizeof (parent));
    basename = path;
  }

  do 
  {
    snprintf (newdir, ndlen, "%s/%s", parent, ".muttXXXXXX");
    mktemp (newdir);
  } 
  while ((rv = mkdir (newdir, 0700)) == -1 && errno == EEXIST);
  
  if (rv == -1)
    return -1;
  
  snprintf (newfile, nflen, "%s/%s", newdir, NONULL(basename));
  return 0;  
}

int mutt_put_file_in_place (const char *path, const char *safe_file, const char *safe_dir)
{
  int rv;
  
  rv = safe_rename (safe_file, path);
  unlink (safe_file);
  rmdir (safe_dir);
  return rv;
}

int safe_open (const char *path, int flags)
{
  struct stat osb, nsb;
  int fd;

  if (flags & O_EXCL) 
  {
    char safe_file[_POSIX_PATH_MAX];
    char safe_dir[_POSIX_PATH_MAX];

    if (mutt_mkwrapdir (path, safe_file, sizeof (safe_file),
			safe_dir, sizeof (safe_dir)) == -1)
      return -1;
    
    if ((fd = open (safe_file, flags, 0600)) < 0)
    {
      rmdir (safe_dir);
      return fd;
    }
    
    if (mutt_put_file_in_place (path, safe_file, safe_dir) == -1)
    {
      close (fd);
      return -1;
    }
  }
  else
  {
    if ((fd = open (path, flags, 0600)) < 0)
      return fd;
  }
    
  /* make sure the file is not symlink */
  if (lstat (path, &osb) < 0 || fstat (fd, &nsb) < 0 ||
      compare_stat(&osb, &nsb) == -1)
  {
/*    dprint (1, (debugfile, "safe_open(): %s is a symlink!\n", path)); */
    close (fd);
    return (-1);
  }

  return (fd);
}

/* when opening files for writing, make sure the file doesn't already exist
 * to avoid race conditions.
 */
FILE *safe_fopen (const char *path, const char *mode)
{
  if (mode[0] == 'w')
  {
    int fd;
    int flags = O_CREAT | O_EXCL;

#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif

    if (mode[1] == '+')
      flags |= O_RDWR;
    else
      flags |= O_WRONLY;

    if ((fd = safe_open (path, flags)) < 0)
      return (NULL);

    return (fdopen (fd, mode));
  }
  else
    return (fopen (path, mode));
}

static char safe_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+@{}._-:%/";

void mutt_sanitize_filename (char *f, short slash)
{
  if (!f) return;

  for (; *f; f++)
  {
    if ((slash && *f == '/') || !strchr (safe_chars, *f))
      *f = '_';
  }
}

/* these characters must be escaped in regular expressions */

static char rx_special_chars[] = "^.[$()|*+?{\\";

int mutt_rx_sanitize_string (char *dest, size_t destlen, const char *src)
{
  while (*src && --destlen > 2)
  {
    if (strchr (rx_special_chars, *src))
    {
      *dest++ = '\\';
      destlen--;
    }
    *dest++ = *src++;
  }
  
  *dest = '\0';
  
  if (*src)
    return -1;
  else
    return 0;
}

/* Read a line from ``fp'' into the dynamically allocated ``s'',
 * increasing ``s'' if necessary. The ending "\n" or "\r\n" is removed.
 * If a line ends with "\", this char and the linefeed is removed,
 * and the next line is read too.
 */
char *mutt_read_line (char *s, size_t *size, FILE *fp, int *line)
{
  size_t offset = 0;
  char *ch;

  if (!s)
  {
    s = safe_malloc (STRING);
    *size = STRING;
  }

  FOREVER
  {
    if (fgets (s + offset, *size - offset, fp) == NULL)
    {
      FREE (&s);
      return NULL;
    }
    if ((ch = strchr (s + offset, '\n')) != NULL)
    {
      (*line)++;
      *ch = 0;
      if (ch > s && *(ch - 1) == '\r')
	*--ch = 0;
      if (ch == s || *(ch - 1) != '\\')
	return s;
      offset = ch - s - 1;
    }
    else
    {
      int c;
      c = getc (fp); /* This is kind of a hack. We want to know if the
                        char at the current point in the input stream is EOF.
                        feof() will only tell us if we've already hit EOF, not
                        if the next character is EOF. So, we need to read in
                        the next character and manually check if it is EOF. */
      if (c == EOF)
      {
        /* The last line of fp isn't \n terminated */
        (*line)++;
        return s;
      }
      else
      {
        ungetc (c, fp); /* undo our dammage */
        /* There wasn't room for the line -- increase ``s'' */
        offset = *size - 1; /* overwrite the terminating 0 */
        *size += STRING;
        safe_realloc (&s, *size);
      }
    }
  }
}

char *
mutt_substrcpy (char *dest, const char *beg, const char *end, size_t destlen)
{
  size_t len;

  len = end - beg;
  if (len > destlen - 1)
    len = destlen - 1;
  memcpy (dest, beg, len);
  dest[len] = 0;
  return dest;
}

char *mutt_substrdup (const char *begin, const char *end)
{
  size_t len;
  char *p;

  if (end)
    len = end - begin;
  else
    len = strlen (begin);

  p = safe_malloc (len + 1);
  memcpy (p, begin, len);
  p[len] = 0;
  return p;
}

/* prepare a file name to survive the shell's quoting rules.
 * From the Unix programming FAQ by way of Liviu.
 */

size_t mutt_quote_filename (char *d, size_t l, const char *f)
{
  size_t i, j = 0;

  if(!f) 
  {
    *d = '\0';
    return 0;
  }

  /* leave some space for the trailing characters. */
  l -= 6;
  
  d[j++] = '\'';
  
  for(i = 0; j < l && f[i]; i++)
  {
    if(f[i] == '\'' || f[i] == '`')
    {
      d[j++] = '\'';
      d[j++] = '\\';
      d[j++] = f[i];
      d[j++] = '\'';
    }
    else
      d[j++] = f[i];
  }
  
  d[j++] = '\'';
  d[j]   = '\0';
  
  return j;
}

/* NULL-pointer aware string comparison functions */

int mutt_strcmp(const char *a, const char *b)
{
  return strcmp(NONULL(a), NONULL(b));
}

int mutt_strcasecmp(const char *a, const char *b)
{
  return strcasecmp(NONULL(a), NONULL(b));
}

int mutt_strncmp(const char *a, const char *b, size_t l)
{
  return strncmp(NONULL(a), NONULL(b), l);
}

int mutt_strncasecmp(const char *a, const char *b, size_t l)
{
  return strncasecmp(NONULL(a), NONULL(b), l);
}

size_t mutt_strlen(const char *a)
{
  return a ? strlen (a) : 0;
}

int mutt_strcoll(const char *a, const char *b)
{
  return strcoll(NONULL(a), NONULL(b));
}

const char *mutt_stristr (const char *haystack, const char *needle)
{
  const char *p, *q;

  if (!haystack)
    return NULL;
  if (!needle)
    return (haystack);

  while (*(p = haystack))
  {
    for (q = needle;
         *p && *q &&
           tolower ((unsigned char) *p) == tolower ((unsigned char) *q);
         p++, q++)
      ;
    if (!*q)
      return (haystack);
    haystack++;
  }
  return NULL;
}

char *mutt_skip_whitespace (char *p)
{
  SKIPWS (p);
  return p;
}

void mutt_remove_trailing_ws (char *s)
{
  char *p;
  
  for (p = s + mutt_strlen (s) - 1 ; p >= s && ISSPACE (*p) ; p--)
    *p = 0;
}

char *mutt_concat_path (char *d, const char *dir, const char *fname, size_t l)
{
  const char *fmt = "%s/%s";
  
  if (!*fname || (*dir && dir[strlen(dir)-1] == '/'))
    fmt = "%s%s";
  
  snprintf (d, l, fmt, dir, fname);
  return d;
}

const char *mutt_basename (const char *f)
{
  const char *p = strrchr (f, '/');
  if (p)
    return p + 1;
  else
    return f;
}

const char *
mutt_strsysexit(int e)
{
  int i;
  
  for(i = 0; sysexits_h[i].str; i++)
  {
    if(e == sysexits_h[i].v)
      break;
  }
  
  return sysexits_h[i].str;
}
