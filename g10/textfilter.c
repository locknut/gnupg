/* textfilter.c
 *	Copyright (C) 1998,1999 Free Software Foundation, Inc.
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include "errors.h"
#include "iobuf.h"
#include "memory.h"
#include "util.h"
#include "filter.h"
#include "i18n.h"


#define MAX_LINELEN 19995 /* a little bit smaller than in armor.c */
			  /* to make sure that a warning is displayed while */
			  /* creating a message */

unsigned
len_without_trailing_ws( byte *line, unsigned len )
{
    byte *p, *mark;
    unsigned n;

    for(mark=NULL, p=line, n=0; n < len; n++, p++ ) {
	if( strchr(" \t\r\n", *p ) ) {
	    if( !mark )
		mark = p;
	}
	else
	    mark = NULL;
    }

    return mark? (mark - line) : len;
}




static int
standard( text_filter_context_t *tfx, IOBUF a,
	  byte *buf, size_t size, size_t *ret_len)
{
    int rc=0;
    size_t len = 0;
    unsigned maxlen;

    assert( size > 10 );
    size -= 2;	/* reserve 2 bytes to append CR,LF */
    while( !rc && len < size ) {
	int lf_seen;

	while( len < size && tfx->buffer_pos < tfx->buffer_len )
	    buf[len++] = tfx->buffer[tfx->buffer_pos++];
	if( len >= size )
	    continue;

	/* read the next line */
	maxlen = MAX_LINELEN;
	tfx->buffer_pos = 0;
	tfx->buffer_len = iobuf_read_line( a, &tfx->buffer,
					   &tfx->buffer_size, &maxlen );
	if( !maxlen )
	    tfx->truncated++;
	if( !tfx->buffer_len ) {
	    if( !len )
		rc = -1; /* eof */
	    break;
	}
	lf_seen = tfx->buffer[tfx->buffer_len-1] == '\n';
	tfx->buffer_len = trim_trailing_ws( tfx->buffer, tfx->buffer_len );
	if( lf_seen ) {
	    tfx->buffer[tfx->buffer_len++] = '\r';
	    tfx->buffer[tfx->buffer_len++] = '\n';
	}
    }
    *ret_len = len;
    return rc;
}

static int
clearsign( text_filter_context_t *tfx, IOBUF a,
	    byte *buf, size_t size, size_t *ret_len)
{
    int rc=0;
    size_t len = 0;
    unsigned maxlen;

    assert( size > 2 );
    size -= 3;	/* reserve for dash escaping and extra LF */
    while( !rc && len < size ) {
	unsigned n;
	byte *p;

	if( tfx->pending_esc ) {
	    buf[len++] = '-';
	    buf[len++] = ' ';
	    tfx->pending_esc = 0;
	}
	while( len < size && tfx->buffer_pos < tfx->buffer_len )
	    buf[len++] = tfx->buffer[tfx->buffer_pos++];
	if( len >= size )
	    continue;

	/* read the next line */
	maxlen = MAX_LINELEN;
	tfx->buffer_pos = 0;
	tfx->buffer_len = iobuf_read_line( a, &tfx->buffer,
					   &tfx->buffer_size, &maxlen );
	p = tfx->buffer;
	n = tfx->buffer_len;
	if( !maxlen )
	    tfx->truncated++;
	if( !n ) { /* readline has returned eof */
	    /* don't hash a pending lf here because the last one is
	     * not part of the signed material. OpenPGP does not
	     * hash the last LF because it may have to add an
	     * extra one in case that the original material
	     * does not end with one.  The clear signed text
	     * must end in a LF, so that the following armor
	     * line can be detected by the parser
	     */
	    if( !tfx->pending_lf ) {
		/* make sure that the file ends with a LF */
		buf[len++] = '\n';
		if( tfx->not_dash_escaped )
		    md_putc(tfx->md, '\n' );
		tfx->pending_lf = 1;
	    }
	    if( !len )
		rc = -1; /* eof */
	    break;
	}
	if( tfx->md ) {
	    if( tfx->not_dash_escaped )
		md_write( tfx->md, p, n );
	    else {
		if( tfx->pending_lf ) {
		    md_putc(tfx->md, '\r' );
		    md_putc(tfx->md, '\n' );
		}
		md_write( tfx->md, p, len_without_trailing_ws( p, n ) );
	    }
	}
	tfx->pending_lf = p[n-1] == '\n';
	if( tfx->not_dash_escaped )
	    ;
	else if( *p == '-' )
	    tfx->pending_esc = 1;
	else if( tfx->escape_from && n > 4 && !memcmp(p, "From ", 5 ) )
	    tfx->pending_esc = 1;
    }
    *ret_len = len;
    return rc;
}


/****************
 * The filter is used to make canonical text: Lines are terminated by
 * CR, LF, trailing white spaces are removed.
 */
int
text_filter( void *opaque, int control,
	     IOBUF a, byte *buf, size_t *ret_len)
{
    size_t size = *ret_len;
    text_filter_context_t *tfx = opaque;
    int rc=0;

    if( control == IOBUFCTRL_UNDERFLOW ) {
	if( tfx->clearsign )
	    rc = clearsign( tfx, a, buf, size, ret_len );
	else
	    rc = standard( tfx, a, buf, size, ret_len );
    }
    else if( control == IOBUFCTRL_FREE ) {
	if( tfx->truncated )
	    log_error(_("can't handle text lines longer than %d characters\n"),
			MAX_LINELEN );
	m_free( tfx->buffer );
	tfx->buffer = NULL;
    }
    else if( control == IOBUFCTRL_DESC )
	*(char**)buf = "text_filter";
    return rc;
}



