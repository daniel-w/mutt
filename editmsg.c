/*
 * Copyright (C) 1999-2000 Thomas Roessler <roessler@guug.de>
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
 *     Software Foundation, Inc., 59 Temple Place - Suite 330,
 *     Boston, MA  02111, USA.
 */ 

/* simple, editor-based message editing */

#include "mutt.h"
#include "copy.h"
#include "mailbox.h"
#include "mx.h"

#include <sys/stat.h>
#include <errno.h>

#include <time.h>

/*
 * return value:
 * 
 * 1	message not modified
 * 0	message edited successfully
 * -1   error
 */

static int edit_one_message (CONTEXT *ctx, HEADER *cur)
{
  char tmp[_POSIX_PATH_MAX];
  char buff[STRING];
  int omagic;
  int oerrno;
  int rc;

  unsigned short o_read;
  unsigned short o_old;

  int of, cf;
  
  CONTEXT tmpctx;
  MESSAGE *msg;

  FILE *fp = NULL;

  struct stat sb;
  time_t mtime = 0;
  
  mutt_mktemp (tmp);

  omagic = DefaultMagic;
  DefaultMagic = M_MBOX;

  rc = (mx_open_mailbox (tmp, M_NEWFOLDER, &tmpctx) == NULL) ? -1 : 0;

  DefaultMagic = omagic;

  if (rc == -1)
  {
    mutt_error (_("could not create temporary folder: %s"), strerror (errno));
    return -1;
  }

  rc = mutt_append_message (&tmpctx, ctx, cur, 0, CH_NOLEN | CH_NOSTATUS); oerrno = errno;

  mx_close_mailbox (&tmpctx, NULL);

  if (rc == -1)
  {
    mutt_error (_("could not write temporary mail folder: %s"), strerror (oerrno));
    goto bail;
  }

  if (stat (tmp, &sb) == 0)
    mtime = sb.st_mtime;

  mutt_edit_file (NONULL(Editor), tmp);

  if ((rc = stat (tmp, &sb)) == -1)
  {
    mutt_error (_("Can't stat %s: %s"), tmp, strerror (errno));
    goto bail;
  }
  
  if (sb.st_size == 0)
  {
    mutt_message (_("Message file is empty!"));
    rc = 1;
    goto bail;
  }

  if (sb.st_mtime == mtime)
  {
    mutt_message (_("Message not modified!"));
    rc = 1;
    goto bail;
  }

  if ((fp = fopen (tmp, "r")) == NULL)
  {
    rc = -1;
    mutt_error (_("Can't open message file: %s"), strerror (errno));
    goto bail;
  }

  if (mx_open_mailbox (ctx->path, M_APPEND, &tmpctx) == NULL)
  {
    rc = -1;
    mutt_error (_("Can't append to folder: %s"), strerror (errno));
    goto bail;
  }

  of = cf = 0;
  
  if (fgets (buff, sizeof (buff), fp) && is_from (buff, NULL, 0, NULL))
  {
    if (tmpctx.magic == M_MBOX || tmpctx.magic == M_MMDF ||
	tmpctx.magic == M_KENDRA)
      cf = CH_FROM | CH_FORCE_FROM;
  }
  else
    of = M_ADD_FROM;

  /* 
   * XXX - we have to play games with the message flags to avoid
   * problematic behaviour with maildir folders.
   *
   */

  o_read = cur->read; o_old = cur->old;
  cur->read = cur->old = 0;
  msg = mx_open_new_message (&tmpctx, cur, of);
  cur->read = o_read; cur->old = o_old;

  if (msg == NULL)
  {
    mutt_error (_("Can't append to folder: %s"), strerror (errno));
    mx_close_mailbox (&tmpctx, NULL);
    goto bail;
  }

  if ((rc = mutt_copy_hdr (fp, msg->fp, 0, sb.st_size, CH_NOSTATUS | CH_NOLEN | cf, NULL)) == 0)
  {
    fputc ('\n', msg->fp);
    rc = mutt_copy_stream (fp, msg->fp);
  }

  rc = mx_commit_message (msg, &tmpctx);
  mx_close_message (&msg);
  
  mx_close_mailbox (&tmpctx, NULL);
  
  bail:
  if (fp) fclose (fp);

  if (rc >= 0)
    unlink (tmp);

  if (rc == 0)
  {
    mutt_set_flag (Context, cur, M_DELETE, 1);
    mutt_set_flag (Context, cur, M_READ, 1);

    if (option (OPTDELETEUNTAG))
      mutt_set_flag (Context, cur, M_TAG, 0);
  }
  else if (rc == -1)
    mutt_message (_("Error. Preserving temporary file: %s"), tmp);

    
  return rc;
}

int mutt_edit_message (CONTEXT *ctx, HEADER *hdr)
{
  int i, j;

  if (hdr)
    return edit_one_message (ctx, hdr);

  
  for (i = 0; i < ctx->vcount; i++)
  {
    j = ctx->v2r[i];
    if (ctx->hdrs[j]->tagged)
    {
      if (edit_one_message (ctx, ctx->hdrs[j]) == -1)
	return -1;
    }
  }

  return 0;
}
