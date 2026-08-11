/*-------------------------------------------------------------------------
 *
 * gp_segment_configuration_internal.h
 *    the shared catalog behind the "catalog" topology provider
 *
 * Portions Copyright (c) 2006-2011, Greenplum Inc.
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	    src/include/catalog/gp_segment_configuration_internal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_SEGMENT_CONFIGURATION_INTERNAL_H
#define GP_SEGMENT_CONFIGURATION_INTERNAL_H

#include "catalog/genbki.h"
#include "catalog/gp_segment_configuration_internal_d.h"

/*
 * Defines for the topology catalog.
 *
 * The relation is gp_segment_configuration_internal; gp_segment_configuration
 * is the view over whatever provider gp_topology_source names, and is what
 * everything outside src/backend/cdb/cdbtopology_catalog.c should read.
 *
 * GpSegmentConfigRelationName deliberately keeps the *view's* name: its only
 * uses are the text of errors about the topology, which are raised by
 * provider-agnostic code and should not name a relation the operator has never
 * heard of and that, under another provider, holds nothing.
 */
#define GpSegmentConfigRelationName		"gp_segment_configuration"

#define COORDINATOR_CONTENT_ID (-1)

#define GP_SEGMENT_CONFIGURATION_ROLE_PRIMARY 'p'
#define GP_SEGMENT_CONFIGURATION_ROLE_MIRROR 'm'

#define GP_SEGMENT_CONFIGURATION_STATUS_UP 'u'
#define GP_SEGMENT_CONFIGURATION_STATUS_DOWN 'd'

#define GP_SEGMENT_CONFIGURATION_MODE_INSYNC 's'
#define GP_SEGMENT_CONFIGURATION_MODE_NOTINSYNC 'n'

/* ----------------
 *		gp_segment_configuration_internal definition.  cpp turns this into
 *		typedef struct FormData_gp_segment_configuration_internal
 *
 * The OID and the OID macro are unchanged on purpose: IsSharedRelation(), the
 * gpexpand event trigger, and the DR redo filter and gg_walfilter -- which key
 * on the raw numbers 5036, 7139, 7140, 6092 and 6093 -- all keep working
 * untouched.  Only the name moves.
 * ----------------
 */
CATALOG(gp_segment_configuration_internal,5036,GpSegmentConfigRelationId) BKI_SHARED_RELATION
{
	int16		dbid;				/* up to 32767 segment databases */
	int16		content;			/* up to 32767 contents -- only 16384 usable with mirroring (see dbid) */

	char		role;
	char		preferred_role;
	char		mode;
	char		status;
	int32		port;

#ifdef CATALOG_VARLEN
	text		hostname;
	text		address;

	text		datadir;
#endif
} FormData_gp_segment_configuration_internal;

/* no foreign keys */

/* ----------------
 *		Form_gp_segment_configuration_internal corresponds to a pointer to a
 *		tuple with the format of the gp_segment_configuration_internal relation.
 * ----------------
 */
typedef FormData_gp_segment_configuration_internal *Form_gp_segment_configuration_internal;

#endif /*_GP_SEGMENT_CONFIGURATION_INTERNAL_H_*/
