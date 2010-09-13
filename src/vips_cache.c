/* Call vips functions, cache recent results
 */

/*

    Copyright (C) 1991-2003 The National Gallery

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

 */

/*

    These files are distributed with VIPS - http://www.vips.ecs.soton.ac.uk

 */

#include "ip.h"

/*
 */
#define DEBUG_TIME
#define DEBUG_HISTORY_SANITY
#define DEBUG_HISTORY_MISS
#define DEBUG
#define DEBUG_HISTORY

/* This is usually turned on from a -D in cflags.
#define DEBUG_LEAK
 */

/* Often want it off ... we get spurious complaints about leaks if an
 * operation has no images in or out (eg. im_version) because it'll never
 * get GCed.
#undef DEBUG_LEAK
 */

/* The previous function calls we are caching, plus an LRU queue for flushing.
 */
static GHashTable *vips_history_table = NULL;
static Queue *vips_history_lru = NULL;
int vips_history_size = 0;

/* Hash from a vargv ... just look at input args and the function name.
 */
static unsigned int
vips_hash( VipsInfo *vi )
{
	int i;
	unsigned int hash;

	if( vi->found_hash )
		return( vi->hash );

	hash = 0;

/* add ints, floats, pointers and strings to the hash.

	FIXME ... could do better on double? could or top and bottom 32 bits
	but would this be stupid on a 64 bit machine?

 */
#define HASH_I( I ) hash = (hash << 1) | ((unsigned int) (I));
#define HASH_D( D ) hash = (hash << 1) | ((unsigned int) (D));
#define HASH_P( P ) hash = (hash << 1) | (GPOINTER_TO_UINT( P ));
#define HASH_S( S ) hash = (hash << 1) | g_str_hash( S );

	/* Add the function to the hash. We often call many functions on
	 * the same args, we'd like these calls to hash to different numbers.
	 */
	HASH_P( vi->fn );

        for( i = 0; i < vi->fn->argc; i++ ) {
                im_type_desc *ty = vi->fn->argv[i].desc;

                if( vips_type_needs_input( ty ) ) {
			switch( vips_lookup_type( ty->type ) ) {
			case VIPS_DOUBLE:
				HASH_D( *((double *) vi->vargv[i]) );
				break;

			case VIPS_INT:
				HASH_I( *((int *) vi->vargv[i]) );
				break;

			case VIPS_COMPLEX:
				HASH_D( ((double *) vi->vargv[i])[0] );
				HASH_D( ((double *) vi->vargv[i])[1] );
				break;

			case VIPS_STRING:
				HASH_S( (char *) vi->vargv[i] );
				break;

			case VIPS_GVALUE:
			case VIPS_INTERPOLATE:
			case VIPS_IMAGE:
				HASH_P( vi->vargv[i] );
				break;

			case VIPS_DOUBLEVEC:
			{
				im_doublevec_object *v = 
					(im_doublevec_object *) vi->vargv[i];
				int j;

				for( j = 0; j < v->n; j++ )
					HASH_D( v->vec[j] );

				break;
			}

			case VIPS_INTVEC:
			{
				im_intvec_object *v = 
					(im_intvec_object *) vi->vargv[i];
				int j;

				for( j = 0; j < v->n; j++ )
					HASH_I( v->vec[j] );

				break;
			}

			case VIPS_DMASK:
			{
				im_mask_object *mo = vi->vargv[i];
				DOUBLEMASK *mask = mo->mask;

				/* mask can be NULL if we are called after 
				 * vips_new() but before we've built the arg
				 * list.
				 */
				if( mask ) {
					int ne = mask->xsize * mask->ysize;
					int j;

					for( j = 0; j < ne; j++ )
						HASH_D( mask->coeff[j] );
					HASH_D( mask->scale );
					HASH_D( mask->offset );
				}

				break;
			}

			case VIPS_IMASK:
			{
				im_mask_object *mo = vi->vargv[i];
				INTMASK *mask = mo->mask;

				/* mask can be NULL if we are called after 
				 * vips_new() but before we've built the arg
				 * list.
				 */
				if( mask ) {
					int ne = mask->xsize * mask->ysize;
					int j;

					for( j = 0; j < ne; j++ )
						HASH_I( mask->coeff[j] );
					HASH_I( mask->scale );
					HASH_I( mask->offset );
				}

				break;
			}

			case VIPS_IMAGEVEC:
			{
				im_imagevec_object *v = 
					(im_imagevec_object *) vi->vargv[i];
				int j;

				for( j = 0; j < v->n; j++ )
					HASH_P( v->vec[j] );

				break;
			}

			default:
			case VIPS_NONE:
				break;
			}
		}
        }

	vi->found_hash = TRUE;
	vi->hash = hash;

	return( hash );
}

/* Are two function calls equal. Check the func and the input args.
 */
static gboolean
vips_equal( VipsInfo *vi1, VipsInfo *vi2 ) 
{
	int i;
	im_function *fn = vi1->fn;

	if( vi1 == vi2 )
		return( TRUE );

	if( vi1->fn != vi2->fn )
		return( FALSE );

        for( i = 0; i < fn->argc; i++ ) {
                im_type_desc *ty = fn->argv[i].desc;

                if( vips_type_needs_input( ty ) ) {
			switch( vips_lookup_type( ty->type ) ) {
			case VIPS_DOUBLE:
				if( *((double *) vi1->vargv[i]) != 
					*((double *) vi2->vargv[i]) )
					return( FALSE );
				break;

			case VIPS_INT:
				if( *((int *) vi1->vargv[i]) != 
					*((int *) vi2->vargv[i]) )
					return( FALSE );
				break;

			case VIPS_COMPLEX:
				if( ((double *) vi1->vargv[i])[0] != 
					((double *) vi2->vargv[i])[0] )
					return( FALSE );
				if( ((double *) vi1->vargv[i])[1] != 
					((double *) vi2->vargv[i])[1] )
					return( FALSE );
				break;

			case VIPS_STRING:
				if( strcmp( (char *) vi1->vargv[i],
					(char *) vi2->vargv[i] ) != 0 )
					return( FALSE );
				break;

			case VIPS_IMAGE:
				if( vi1->vargv[i] != vi2->vargv[i] )
					return( FALSE );
				break;

			case VIPS_DOUBLEVEC:
			{
				im_doublevec_object *v1 = 
					(im_doublevec_object *) vi1->vargv[i];
				im_doublevec_object *v2 = 
					(im_doublevec_object *) vi2->vargv[i];
				int j;

				for( j = 0; j < v1->n; j++ )
					if( v1->vec[j] != v2->vec[j] )
						return( FALSE );

				break;
			}

			case VIPS_INTVEC:
			{
				im_intvec_object *v1 = 
					(im_intvec_object *) vi1->vargv[i];
				im_intvec_object *v2 = 
					(im_intvec_object *) vi2->vargv[i];
				int j;

				for( j = 0; j < v1->n; j++ )
					if( v1->vec[j] != v2->vec[j] )
						return( FALSE );

				break;
			}

			case VIPS_DMASK:
			{
				im_mask_object *mo1 = 
					(im_mask_object *) vi1->vargv[i];
				im_mask_object *mo2 = 
					(im_mask_object *) vi2->vargv[i];
				DOUBLEMASK *mask1 = mo1->mask;
				DOUBLEMASK *mask2 = mo2->mask;
				int ne = mask1->xsize * mask2->ysize;
				int j;
	
				if( mask1->xsize != mask2->xsize ||
					mask1->ysize != mask2->ysize )
					return( FALSE );

				for( j = 0; j < ne; j++ )
					if( mask1->coeff[j] != mask2->coeff[j] )
						return( FALSE );

				if( mask1->scale != mask2->scale )
					return( FALSE );
				if( mask1->offset != mask2->offset ) 
					return( FALSE );

				break;
			}

			case VIPS_IMASK:
			{
				im_mask_object *mo1 = 
					(im_mask_object *) vi1->vargv[i];
				im_mask_object *mo2 = 
					(im_mask_object *) vi2->vargv[i];
				INTMASK *mask1 = mo1->mask;
				INTMASK *mask2 = mo2->mask;
				int ne = mask1->xsize * mask2->ysize;
				int j;
	
				if( mask1->xsize != mask2->xsize ||
					mask1->ysize != mask2->ysize )
					return( FALSE );

				for( j = 0; j < ne; j++ )
					if( mask1->coeff[j] != mask2->coeff[j] )
						return( FALSE );

				if( mask1->scale != mask2->scale )
					return( FALSE );
				if( mask1->offset != mask2->offset ) 
					return( FALSE );

				break;
			}

			case VIPS_IMAGEVEC:
			{
				im_imagevec_object *v1 = 
					(im_imagevec_object *) vi1->vargv[i];
				im_imagevec_object *v2 = 
					(im_imagevec_object *) vi2->vargv[i];
				int j;

				for( j = 0; j < v1->n; j++ )
					if( v1->vec[j] != v2->vec[j] )
						return( FALSE );

				break;
			}

			/* Very strict. Could be more generous here: we'd need
			 * to have a pspec for each argument type and then use 
			 * g_param_values_cmp() to test equality.
			 */
			case VIPS_GVALUE:
				if( vi1->vargv[i] != vi2->vargv[i] )
					return( FALSE );
				break;

			case VIPS_INTERPOLATE:
				if( vi1->vargv[i] != vi2->vargv[i] )
					return( FALSE );
				break;

			default:
			case VIPS_NONE:
				break;
			}
		}
        }

	return( TRUE );
}

#ifdef DEBUG_HISTORY_SANITY
static void
vips_history_sanity_sub( VipsInfo *vi )
{
	g_assert( g_slist_find( vips_history_lru->list, vi ) );
}

static void
vips_history_sanity( void )
{
	GSList *p;

	if( !vips_history_lru || !vips_history_table )
		return;

	/* Everything that's on the LRU should be in the history table.
	 */
	for( p = vips_history_lru->list; p; p = p->next ) {
		VipsInfo *vi = (VipsInfo *) p->data;

		g_assert( g_hash_table_lookup( vips_history_table, vi ) );

		g_assert( vi->fn );
		g_assert( vi->fn->argc > 0 && vi->fn->argc < MAX_VIPS_ARGS );
		g_assert( vi->in_cache );
	}

	/* Everything that's on the history table should be in the LRU.
	 */
	g_hash_table_foreach( vips_history_table,
		(GHFunc) vips_history_sanity_sub, NULL );

	/* Everything that's on the LRU should be in the global vips_info
	 * list.
	 */
	for( p = vips_history_lru->list; p; p = p->next ) {
		VipsInfo *vi = (VipsInfo *) p->data;

		/* Need to build with DEBUG on before vips_info_all is
		 * maintained.
		 */
		g_assert( g_slist_find( vips_info_all, vi ) );
	}

	/* Everything on vips_info_all that's not in vips_history_table should
	 * have in_cache FALSE.
	 */
	for( p = vips_info_all; p; p = p->next ) {
		VipsInfo *vi = (VipsInfo *) p->data;

		if( !g_hash_table_lookup( vips_history_table, vi ) )
			g_assert( !vi->in_cache );
	}

	/* Every image argument on every in-cache call should be sane.
	 */
	for( p = vips_info_all; p; p = p->next ) {
		VipsInfo *vi = (VipsInfo *) p->data;
		int i;

		if( !vi->in_cache )
			continue;

		for( i = 0; i < vi->fn->argc; i++ ) {
			im_type_desc *ty = vi->fn->argv[i].desc;

			if( strcmp( ty->type, IM_TYPE_IMAGE ) == 0 ) {
				IMAGE *im = (IMAGE *) vi->vargv[i];

				g_assert( !im_image_sanity( im ) );
			}
			else if( strcmp( ty->type, IM_TYPE_IMAGEVEC ) == 0 ) {
				im_imagevec_object *iv = 
					(im_imagevec_object *) vi->vargv[i];
				int j;

				for( j = 0; j < iv->n; j++) 
					g_assert( !im_image_sanity( 
						iv->vec[j] ) );
			}
		}
	}
}
#endif /*DEBUG_HISTORY_SANITY */

/* Is a function call in our history? Return the old one. 
 */
static VipsInfo *
vips_history_lookup( VipsInfo *vi )
{
	VipsInfo *old_vi;

	if( !vips_history_table ) {
		vips_history_table = g_hash_table_new( 
			(GHashFunc) vips_hash, (GEqualFunc) vips_equal );
		vips_history_lru = queue_new();
	}

	old_vi = (VipsInfo *) g_hash_table_lookup( vips_history_table, vi );

#ifdef DEBUG_HISTORY
	if( old_vi ) 
		printf( "vips_history_lookup: found \"%s\"\n", old_vi->name );
#endif /*DEBUG_HISTORY*/
#ifdef DEBUG_HISTORY_SANITY
	vips_history_sanity();
#endif /*DEBUG_HISTORY_SANITY*/

	return( old_vi );
}

/* Bump to end of LRU.
 */
static void
vips_history_touch( VipsInfo *vi )
{
	g_assert( vi->in_cache );

	queue_remove( vips_history_lru, vi );
	queue_add( vips_history_lru, vi );

#ifdef DEBUG_HISTORY_SANITY
	vips_history_sanity();
#endif /*DEBUG_HISTORY_SANITY*/
#ifdef DEBUG_HISTORY
	printf( "vips_history_touch: bumping \"%s\"\n", vi->name );
#endif /*DEBUG_HISTORY*/
}

static void
vips_history_insert( VipsInfo *vi )
{
	g_assert( !g_hash_table_lookup( vips_history_table, vi ) );
	g_assert( !vi->in_cache );

	g_hash_table_insert( vips_history_table, vi, vi );
	vips_history_size += 1;

	g_assert( g_hash_table_lookup( vips_history_table, vi ) );

	queue_add( vips_history_lru, vi );
	vi->in_cache = TRUE;

#ifdef DEBUG_HISTORY_SANITY
	vips_history_sanity();
#endif /*DEBUG_HISTORY_SANITY*/
#ifdef DEBUG_HISTORY
	printf( "vips_history_insert: adding \"%s\"\n", vi->name );
#endif /*DEBUG_HISTORY*/
}

/* Are we in the history? Remove us.
 */
void
vips_history_remove( VipsInfo *vi )
{
	if( vi->in_cache ) {
		queue_remove( vips_history_lru, vi );
		g_hash_table_remove( vips_history_table, vi );
		vips_history_size -= 1;
		vi->in_cache = FALSE;

#ifdef DEBUG_HISTORY
		printf( "vips_history_remove: removing \"%s\"\n", vi->name );
#endif /*DEBUG_HISTORY*/
	}

#ifdef DEBUG_HISTORY_SANITY
	vips_history_sanity();
#endif /*DEBUG_HISTORY_SANITY*/
}

static void
vips_history_remove_lru( void )
{
	VipsInfo *vi;

	vi = (VipsInfo *) queue_head( vips_history_lru );

#ifdef DEBUG_HISTORY
	printf( "vips_history_remove_lru: flushing \"%s\"\n", vi->name );
#endif /*DEBUG_HISTORY*/

	vips_destroy( vi );

#ifdef DEBUG_HISTORY_SANITY
	vips_history_sanity();
#endif /*DEBUG_HISTORY_SANITY*/
}

static void
vips_history_destroy_cb( Imageinfo *ii, VipsInfo *vi )
{
#ifdef DEBUG_HISTORY
	printf( "vips_history_destroy_cb: on death of ii, uncaching \"%s\"\n", 
		vi->name );
#endif /*DEBUG_HISTORY*/

	vips_destroy( vi );
}

static void
vips_history_invalidate_cb( Imageinfo *ii, VipsInfo *vi )
{
#ifdef DEBUG_HISTORY
	printf( "vips_history_invalidate_cb: "
		"on invalidate of ii, uncaching \"%s\"\n", vi->name );
#endif /*DEBUG_HISTORY*/

	vips_destroy( vi );
}

/* Add a function call to the history. 
 */
static void
vips_history_add( VipsInfo *vi )
{
	int i;

#ifdef DEBUG_HISTORY
	printf( "vips_history_add: adding \"%s\" (%p), hash = %u\n", 
		vi->name, vi, vi->hash );
#endif /*DEBUG_HISTORY*/

	vips_history_insert( vi );

	/* If any of our ii are destroyed, we must go too.
	 */
	for( i = 0; i < vi->noutii; i++ )
		vi->outii_destroy_sid[i] = g_signal_connect( vi->outii[i], 
			"destroy", 
			G_CALLBACK( vips_history_destroy_cb ), vi );

	/* If any of our input ii are destroyed or painted on, we must also 
	 * uncache.
	 */
	for( i = 0; i < vi->ninii; i++ ) {
		vi->inii_destroy_sid[i] = g_signal_connect( vi->inii[i], 
			"destroy", 
			G_CALLBACK( vips_history_destroy_cb ), vi );
		vi->inii_invalidate_sid[i] = g_signal_connect( vi->inii[i], 
			"invalidate", 
			G_CALLBACK( vips_history_invalidate_cb ), vi );
	}

	/* History too big? Flush!
	 */
	if( queue_length( vips_history_lru ) > VIPS_HISTORY_MAX ) 
		vips_history_remove_lru();
}

/* Sort out the input images. 
 */
static void
vips_gather( VipsInfo *vi ) 
{
	int i, j;

	/* Find all the input images.
	 */
	for( i = 0; i < vi->fn->argc; i++ ) {
		im_type_desc *ty = vi->fn->argv[i].desc;

		if( !vips_type_needs_input( ty ) ) 
			continue;

		if( strcmp( ty->type, IM_TYPE_IMAGE ) == 0 ) {
			vi->inii[vi->ninii] = vi->vargv[i];
			vi->ninii += 1;
		}

		if( strcmp( ty->type, IM_TYPE_IMAGEVEC ) == 0 ) {
			im_imagevec_object *iv = 
				(im_imagevec_object *) vi->vargv[i];

			for( j = 0; j < iv->n; j++ ) {
				vi->inii[vi->ninii] = IMAGEINFO( iv->vec[j] );
				vi->ninii += 1;
			}
		}
	}

	/* No input images.
	 */
	if( vi->ninii == 0 )
		return;

	/* Can we LUT? Function needs to be LUTable, all input images
	 * have to be the same underlying image, and image must be uncoded
	 * IM_BANDFMT_UCHAR.
	 */
	vi->use_lut = (vi->fn->flags & IM_FN_PTOP) && 
		imageinfo_same_underlying( vi->inii, vi->ninii ) &&
		imageinfo_get_underlying( vi->inii[0] )->Coding == 
			IM_CODING_NONE &&
		imageinfo_get_underlying( vi->inii[0] )->BandFmt == 
			IM_BANDFMT_UCHAR;

	/* Now zap the vargv vector with the correct IMAGE pointers.
	 */
	for( i = 0; i < vi->fn->argc; i++ ) {
		im_type_desc *ty = vi->fn->argv[i].desc;

		if( !vips_type_needs_input( ty ) ) 
			continue;

		if( strcmp( ty->type, IM_TYPE_IMAGE ) == 0 ) { 
			if( !(vi->vargv[i] = 
				imageinfo_get( vi->use_lut, vi->vargv[i] )) )
				reduce_throw( vi->rc );
		}

		if( strcmp( ty->type, IM_TYPE_IMAGEVEC ) == 0 ) {
			im_imagevec_object *iv = 
				(im_imagevec_object *) vi->vargv[i];

			/* Found an input image vector. Add all the imageinfo
			 * in the vector.
			 */
			for( j = 0; j < iv->n; j++ ) {
				Imageinfo *ii = IMAGEINFO( iv->vec[j] );

				if( !(iv->vec[j] = imageinfo_get( 
					vi->use_lut, ii )) )
					reduce_throw( vi->rc );
			}
		}
	}
}

/* Loop over the output IMAGEs, transforming them into Imageinfo.
 */
static void
vips_wrap_output( VipsInfo *vi )
{
	int i;

	for( i = 0; i < vi->nres; i++ ) {
		int j = vi->outpos[i];
		IMAGE *im = (IMAGE *) vi->vargv[j];
		im_type_desc *ty = vi->fn->argv[j].desc;
		Imageinfo *outii;

		if( vips_lookup_type( ty->type ) != VIPS_IMAGE )
			continue;

		if( !(outii = imageinfo_new( main_imageinfogroup, 
			vi->rc->heap, im, NULL )) )
			reduce_throw( vi->rc );
		if( vi->use_lut ) 
			imageinfo_set_underlying( outii, vi->inii[0] );

		/* This output ii depends upon all of the input images.
		 */
		managed_sub_add_all( MANAGED( outii ), 
			vi->ninii, (Managed **) vi->inii );

		/* Junk the pointer in vargv to stop im_close() on vips end.
		 */
		vi->vargv[j] = NULL;

		/* Rewind the image.
		 */
		if( im_pincheck( im ) ) {
			vips_error( vi );
			reduce_throw( vi->rc );
		}

		/* Record on output ii table.
		 */
		vi->outii[vi->noutii++] = outii;
	}
}

/* Get the args from the VIPS call buffer.
 */
static void
vips_args_vips( VipsInfo *vi, VipsBuf *buf )
{
	int i;

	vips_buf_appendf( buf, _( "You passed:" ) );
	vips_buf_appendf( buf, "\n" );
        for( i = 0; i < vi->fn->argc; i++ ) {
                im_type_desc *ty = vi->fn->argv[i].desc;
                char *name = vi->fn->argv[i].name;

                if( vips_type_needs_input( ty ) ) {
                        vips_buf_appendf( buf, "   %s - ", name );
                        vips_tochar( vi, i, buf );
                        vips_buf_appendf( buf, "\n" );
                }
        }
}

/* There's a problem calling the function. Show args from the vips call
 * struct.
 */
static void
vips_error_fn_vips( VipsInfo *vi )
{
	char txt[1000];
	VipsBuf buf = VIPS_BUF_STATIC( txt );

	error_top( _( "VIPS library error." ) );

	vips_buf_appendf( &buf, 
		_( "Error calling library function \"%s\" (%s)." ),
		vi->name, vi->fn->desc );
	vips_buf_appendf( &buf, "\n" );
	vips_buf_appendf( &buf, _( "VIPS library: %s" ), im_error_buffer() );
	im_error_clear();
	vips_buf_appendf( &buf, "\n" );
	vips_args_vips( vi, &buf );
	vips_buf_appendf( &buf, "\n" );
	vips_usage( &buf, vi->fn );
	error_sub( "%s", vips_buf_all( &buf ) );
}

static gboolean
vips_build_argv( VipsInfo *vi, char **argv )
{
	int i;

	for( i = 0; i < vi->fn->argc; i++ ) {
		char txt[512];
		VipsBuf buf = VIPS_BUF_STATIC( txt );

		vips_tobuf( vi, i, &buf );
		if( !(argv[i] = im_strdup( NULL, vips_buf_all( &buf ) )) )
			return( FALSE );
	}

#ifdef DEBUG
	printf( "vips_build_argv: argv for %s is:\n  ", vi->fn->name );
	for( i = 0; i < vi->fn->argc; i++ )
		printf( "%s ", NN( argv[i] ) );
	printf( "\n" );
#endif /*DEBUG*/

	return( TRUE );
}

static void
vips_free_argv( int argc, char **argv )
{
	int i;

	for( i = 0; i < argc; i++ ) {
		IM_FREE( argv[i] );
	}
	IM_FREE( argv );
}

/* Update the VIPS hist for all output images.
 */
static void
vips_update_hist( VipsInfo *vi )
{
	int argc = vi->fn->argc;
	char **argv;
	int i;

#ifdef DEBUG
	printf( "vips_update_hist: %s\n", vi->name );
#endif /*DEBUG*/

	/* No output images? Nothing to do.
	 */
	if( vi->nires == 0 )
		return;

	/* Build an argv for this call. +1 for NULL termination.
	 */
	if( !(argv = IM_ARRAY( NULL, argc + 1, char * )) )
		return;
	for( i = 0; i < argc + 1; i++ )
		argv[i] = NULL;
	if( !vips_build_argv( vi, argv ) ) {
		vips_free_argv( argc, argv );
		return;
	}

	for( i = 0; i < vi->nres; i++ ) {
		int j = vi->outpos[i];
		im_type_desc *ty = vi->fn->argv[j].desc;

		/* Image output.
		 */
		if( vips_lookup_type( ty->type ) == VIPS_IMAGE ) {
#ifdef DEBUG
			printf( "vips_update_hist: adding to arg %d\n", j );
#endif /*DEBUG*/

			im_updatehist( vi->vargv[j], vi->fn->name, argc, argv );
		}
	}

	vips_free_argv( argc, argv );
}

/* Call a vips operation. 
 *
 * The cache takes ownership of the VipsInfo passed in, and returns a ref to a
 * VipsInfo (might be a different one) that should be unreffed when you're 
 * done with it.
 */
VipsInfo *
vips_dispatch( VipsInfo *vi, PElement *out )
{
	Reduce *rc = vi->rc;
	VipsInfo *old_vi;

	/* Calculate the hash for this vi after building it, but before we do
	 * vips_gather();
	 */
	(void) vips_hash( vi );

	/* Look over the images we have ... turn input Imageinfo to IMAGE.
	 * If we can do this with a lut, set all that up.
	 */
	vips_gather( vi );

	/* Is this function call in the history?
	 */
	if( (old_vi = vips_history_lookup( vi )) ) {
		/* Yes: reuse!
		 */
		vi = old_vi;

		if( trace_flags & TRACE_VIPS ) 
			vips_buf_appendf( trace_current(), "(from cache) " );

#ifdef DEBUG_HISTORY
		printf( "vips_dispatch: found %s in history\n", vi->name );
#endif /*DEBUG_HISTORY*/
	}
	else {
		/* No: call function.
		 */
		int result;

#ifdef DEBUG_TIME
		static GTimer *timer = NULL;

		if( !timer )
			timer = g_timer_new();
		g_timer_reset( timer );
#endif /*DEBUG_TIME*/

#ifdef DEBUG_HISTORY_MISS
		printf( "vips_dispatch: calling %s\n", vi->name );
#endif /*DEBUG_HISTORY_MISS*/

		/* Be careful. Eval callbacks from this may do anything,
		 * including call vips_dispatch().
		 */
		result = vi->fn->disp( vi->vargv );

#ifdef DEBUG_TIME
		printf( "vips_dispatch: %s - %g\n", 
			vi->name, g_timer_elapsed( timer, NULL ) );
#endif /*DEBUG_TIME*/

		if( result ) {
			vips_error_fn_vips( vi );
			return( NULL );
		}
		vips_update_hist( vi );

		/* Transform output IMAGE back into Imageinfo.
		 */
		vips_wrap_output( vi );
	}

	/* Add to our operation cache, if necessary.
	 */
	if( !(vi->fn->flags & IM_FN_NOCACHE) ) {
		if( vi->in_cache ) 
			/* Already in the history. Just touch the time.
			 */
			vips_history_touch( vi );
		else if( (old_vi = vips_history_lookup( vi )) ) {
			/* We have an equal but older item there? This can 
			 * happen with nested calls. Touch the old one.
			 */
			vips_history_touch( old_vi );
			vi = old_vi;
		}
		else
			vips_history_add( vi );
	}

	return( vi );
}
