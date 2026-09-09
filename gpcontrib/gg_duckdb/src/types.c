/*-------------------------------------------------------------------------
 *
 * types.c
 *	  The type firewall: which PostgreSQL types a region may carry, how they
 *	  map onto DuckDB logical types, and the two conversions (Datum into a
 *	  DuckDB vector row, DuckDB vector row into a Datum).
 *
 * Only types whose values round-trip exactly are supported.  Everything
 * else makes the leaf, and hence the region, ineligible.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "libpq/pqformat.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"

#include "gg_duckdb/gg_duckdb.h"

/* days / microseconds between 1970-01-01 (DuckDB epoch) and 2000-01-01 (PG epoch) */
#define GG_EPOCH_DAYS	((int32) (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE))
#define GG_EPOCH_USECS	((int64) GG_EPOCH_DAYS * USECS_PER_DAY)

/* DuckDB's infinities: date_t/timestamp_t use max and -max, PG uses min/max */
#define DUCK_DATE_INF		PG_INT32_MAX
#define DUCK_DATE_NINF		(-PG_INT32_MAX)
#define DUCK_TS_INF			PG_INT64_MAX
#define DUCK_TS_NINF		(-PG_INT64_MAX)

typedef __int128 int128;

/*
 * Fill *ti for a PG type, or return false when the type is outside the
 * whitelist.  numeric needs an explicit typmod with precision <= 38.
 */
bool
gg_duckdb_type_map(Oid typid, int32 typmod, GGTypeInfo *ti)
{
	memset(ti, 0, sizeof(*ti));
	ti->typid = typid;
	ti->typmod = typmod;

	switch (typid)
	{
		case BOOLOID:
			ti->duck = DUCKDB_TYPE_BOOLEAN;
			break;
		case INT2OID:
			ti->duck = DUCKDB_TYPE_SMALLINT;
			break;
		case INT4OID:
			ti->duck = DUCKDB_TYPE_INTEGER;
			break;
		case INT8OID:
			ti->duck = DUCKDB_TYPE_BIGINT;
			break;
		case FLOAT4OID:
			ti->duck = DUCKDB_TYPE_FLOAT;
			break;
		case FLOAT8OID:
			ti->duck = DUCKDB_TYPE_DOUBLE;
			break;
		case NUMERICOID:
			{
				int			precision,
							scale;

				if (typmod < (int32) VARHDRSZ)
					return false;	/* unconstrained numeric: no DECIMAL width */
				precision = ((typmod - VARHDRSZ) >> 16) & 0xffff;
				scale = (typmod - VARHDRSZ) & 0xffff;
				if (precision < 1 || precision > 38 || scale < 0 || scale > precision)
					return false;
				ti->duck = DUCKDB_TYPE_DECIMAL;
				ti->width = precision;
				ti->scale = scale;
				break;
			}
		case TEXTOID:
		case VARCHAROID:
		case BPCHAROID:
			ti->duck = DUCKDB_TYPE_VARCHAR;
			break;
		case BYTEAOID:
			ti->duck = DUCKDB_TYPE_BLOB;
			break;
		case DATEOID:
			ti->duck = DUCKDB_TYPE_DATE;
			break;
		case TIMESTAMPOID:
			ti->duck = DUCKDB_TYPE_TIMESTAMP;
			break;
		case TIMESTAMPTZOID:
			ti->duck = DUCKDB_TYPE_TIMESTAMP_TZ;
			break;
		case INTERVALOID:
			ti->duck = DUCKDB_TYPE_INTERVAL;
			break;
		case UUIDOID:
			ti->duck = DUCKDB_TYPE_UUID;
			break;
		default:
			return false;
	}
	return true;
}

static int128 numeric_text_to_scaled(const char *s, int scale, int width);
static char *scaled_to_numeric_text(int128 v, int scale);

/*
 * The shape of a numeric aggregate state (the bytea a partial sum(numeric)
 * or avg(numeric) hands to its final phase) as DuckDB carries it: a STRUCT
 * of the sum, a DECIMAL(38,scale), and the count of non-null inputs.
 */
void
gg_duckdb_numeric_state_type(GGTypeInfo *ti, int scale)
{
	memset(ti, 0, sizeof(*ti));
	ti->typid = BYTEAOID;
	ti->typmod = -1;
	ti->duck = DUCKDB_TYPE_STRUCT;
	ti->width = 38;
	ti->scale = (uint8) scale;
}

bool
gg_duckdb_is_numeric_state(const GGTypeInfo *ti)
{
	return ti->duck == DUCKDB_TYPE_STRUCT && ti->typid == BYTEAOID;
}

/* The DuckDB logical type for a mapped PG type; caller destroys it. */
duckdb_logical_type
gg_duckdb_logical_type(const GGTypeInfo *ti)
{
	if (ti->duck == DUCKDB_TYPE_DECIMAL)
		return duckdb_create_decimal_type(ti->width, ti->scale);
	if (gg_duckdb_is_numeric_state(ti))
	{
		duckdb_logical_type members[2];
		const char *names[2] = {"s", "n"};
		duckdb_logical_type lt;

		members[0] = duckdb_create_decimal_type(38, ti->scale);
		members[1] = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
		lt = duckdb_create_struct_type(members, names, 2);
		duckdb_destroy_logical_type(&members[0]);
		duckdb_destroy_logical_type(&members[1]);
		return lt;
	}
	return duckdb_create_logical_type(ti->duck);
}

/*
 * Numeric aggregate states in numeric_avg_serialize()'s layout: N, sumX as
 * numeric_send() writes it, maxScale, maxScaleCount, NaNcount.
 */
static void
numeric_state_unpack(Datum d, int scale, bool *sum_null, int128 *sum, int64 *n)
{
	bytea	   *state = DatumGetByteaPP(d);
	StringInfoData buf;
	Datum		sumx;
	int64		nancount;
	char	   *text;

	initStringInfo(&buf);
	appendBinaryStringInfo(&buf, VARDATA_ANY(state), VARSIZE_ANY_EXHDR(state));
	*n = pq_getmsgint64(&buf);
	sumx = DirectFunctionCall3(numeric_recv, PointerGetDatum(&buf),
							   ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
	(void) pq_getmsgint(&buf, 4);	/* maxScale */
	(void) pq_getmsgint64(&buf);	/* maxScaleCount */
	nancount = pq_getmsgint64(&buf);
	if (nancount > 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("gg_duckdb: a numeric aggregate state holding NaN cannot be handed to DuckDB")));
	*sum_null = (*n == 0);
	text = DatumGetCString(DirectFunctionCall1(numeric_out, sumx));
	*sum = *sum_null ? 0 : numeric_text_to_scaled(text, scale, 38);
	pfree(buf.data);
}

static Datum
numeric_state_pack(bool sum_null, int128 sum, int64 n, int scale)
{
	StringInfoData buf;
	Datum		sumx;
	bytea	   *sent;

	if (sum_null)
		n = 0;
	sumx = DirectFunctionCall3(numeric_in, CStringGetDatum(scaled_to_numeric_text(sum_null ? 0 : sum, scale)),
							   ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
	sent = DatumGetByteaPP(DirectFunctionCall1(numeric_send, sumx));
	pq_begintypsend(&buf);
	pq_sendint64(&buf, n);
	pq_sendbytes(&buf, VARDATA_ANY(sent), VARSIZE_ANY_EXHDR(sent));
	pq_sendint32(&buf, scale);		/* maxScale */
	pq_sendint64(&buf, n);			/* maxScaleCount */
	pq_sendint64(&buf, 0);			/* NaNcount */
	return PointerGetDatum(pq_endtypsend(&buf));
}

const char *
gg_duckdb_type_name(duckdb_type t)
{
	switch (t)
	{
		case DUCKDB_TYPE_BOOLEAN: return "BOOLEAN";
		case DUCKDB_TYPE_SMALLINT: return "SMALLINT";
		case DUCKDB_TYPE_INTEGER: return "INTEGER";
		case DUCKDB_TYPE_BIGINT: return "BIGINT";
		case DUCKDB_TYPE_FLOAT: return "FLOAT";
		case DUCKDB_TYPE_DOUBLE: return "DOUBLE";
		case DUCKDB_TYPE_DECIMAL: return "DECIMAL";
		case DUCKDB_TYPE_VARCHAR: return "VARCHAR";
		case DUCKDB_TYPE_BLOB: return "BLOB";
		case DUCKDB_TYPE_STRUCT: return "STRUCT";
		case DUCKDB_TYPE_DATE: return "DATE";
		case DUCKDB_TYPE_TIMESTAMP: return "TIMESTAMP";
		case DUCKDB_TYPE_TIMESTAMP_TZ: return "TIMESTAMP WITH TIME ZONE";
		case DUCKDB_TYPE_INTERVAL: return "INTERVAL";
		case DUCKDB_TYPE_UUID: return "UUID";
		case DUCKDB_TYPE_HUGEINT: return "HUGEINT";
		default: return "?";
	}
}

/* ---------- numeric <-> DECIMAL through the decimal string ---------- */

/*
 * Parse numeric_out() text into a scaled 128-bit integer for DECIMAL(w,s).
 * The value comes from a numeric(p,s) column, so it never carries more than
 * s fractional digits; anything else is a bug worth an error, as is NaN,
 * which DECIMAL cannot hold.
 */
static int128
numeric_text_to_scaled(const char *s, int scale, int width)
{
	const char *p = s;
	bool		neg = false;
	int128		v = 0;
	int			frac = -1;		/* fractional digits consumed, -1 before '.' */
	int			ndigits = 0;

	if (strcmp(s, "NaN") == 0 || strcmp(s, "Infinity") == 0 || strcmp(s, "-Infinity") == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("gg_duckdb: numeric value %s cannot be handed to DuckDB", s)));
	if (*p == '-')
	{
		neg = true;
		p++;
	}
	else if (*p == '+')
		p++;
	for (; *p; p++)
	{
		if (*p == '.')
		{
			frac = 0;
			continue;
		}
		if (*p < '0' || *p > '9')
			elog(ERROR, "gg_duckdb: unexpected numeric text \"%s\"", s);
		if (frac >= 0)
		{
			if (frac >= scale)
			{
				if (*p != '0')
					elog(ERROR, "gg_duckdb: numeric \"%s\" has more than %d fractional digits",
						 s, scale);
				continue;
			}
			frac++;
		}
		v = v * 10 + (*p - '0');
		if (v != 0 || ndigits > 0)
			ndigits++;
	}
	/* pad the missing fractional digits */
	for (frac = frac < 0 ? 0 : frac; frac < scale; frac++)
		v *= 10;
	if (ndigits > width)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("gg_duckdb: numeric \"%s\" does not fit DECIMAL(%d,%d)", s, width, scale)));
	return neg ? -v : v;
}

/* Format a scaled 128-bit integer as decimal text with `scale` fraction digits. */
static char *
scaled_to_numeric_text(int128 v, int scale)
{
	char		digits[64];
	int			n = 0;
	bool		neg = v < 0;
	StringInfoData buf;
	int			i;

	if (neg)
		v = -v;
	do
	{
		digits[n++] = '0' + (int) (v % 10);
		v /= 10;
	} while (v != 0);
	/* at least scale + 1 digits so that "0.xx" forms */
	while (n < scale + 1)
		digits[n++] = '0';

	initStringInfo(&buf);
	if (neg)
		appendStringInfoChar(&buf, '-');
	for (i = n - 1; i >= 0; i--)
	{
		appendStringInfoChar(&buf, digits[i]);
		if (i == scale && scale > 0)
			appendStringInfoChar(&buf, '.');
	}
	return buf.data;
}

/* ---------- writing a Datum into row `row` of a DuckDB vector ---------- */

/* DuckDB's validity mask: one bit per row, 64 rows per word, set = valid. */
static inline void
validity_set(uint64_t *validity, idx_t row, bool valid)
{
	uint64_t   *word = &validity[row / 64];
	uint64_t	bit = UINT64CONST(1) << (row % 64);

	if (valid)
		*word |= bit;
	else
		*word &= ~bit;
}

/*
 * Resolve the vector's data and validity (made writable here) and, for a
 * numeric aggregate state, its children's, so that rows can be written
 * without calling DuckDB.
 */
void
gg_duckdb_vector_target(duckdb_vector vec, const GGTypeInfo *ti, GGVectorTarget *t)
{
	memset(t, 0, sizeof(*t));
	t->vec = vec;
	t->data = duckdb_vector_get_data(vec);
	duckdb_vector_ensure_validity_writable(vec);
	t->validity = duckdb_vector_get_validity(vec);
	if (ti->duck == DUCKDB_TYPE_STRUCT)
	{
		duckdb_vector sv = duckdb_struct_vector_get_child(vec, 0);
		duckdb_vector nv = duckdb_struct_vector_get_child(vec, 1);

		t->sum_data = duckdb_vector_get_data(sv);
		duckdb_vector_ensure_validity_writable(sv);
		t->sum_validity = duckdb_vector_get_validity(sv);
		t->count_data = duckdb_vector_get_data(nv);
	}
}

/*
 * Write one value.  Everything is a plain memory write into the target
 * except strings, which are handed back through `str`/`len` (pointing into
 * the datum, which the caller keeps alive) for the caller to give DuckDB
 * once it is outside its PG_TRY block: DuckDB's string heap allocates, and
 * an allocation failure is a C++ exception that would unwind through the
 * block and leave PostgreSQL's exception stack pointing into a dead frame.
 */
void
gg_duckdb_write_datum(const GGTypeInfo *ti, const GGVectorTarget *t, idx_t row,
					  Datum d, bool isnull, const char **str, int *len)
{
	void	   *data = t->data;

	*str = NULL;
	*len = -1;
	if (isnull)
	{
		validity_set(t->validity, row, false);
		return;
	}
	validity_set(t->validity, row, true);

	switch (ti->duck)
	{
		case DUCKDB_TYPE_BOOLEAN:
			((bool *) data)[row] = DatumGetBool(d);
			break;
		case DUCKDB_TYPE_SMALLINT:
			((int16 *) data)[row] = DatumGetInt16(d);
			break;
		case DUCKDB_TYPE_INTEGER:
			((int32 *) data)[row] = DatumGetInt32(d);
			break;
		case DUCKDB_TYPE_BIGINT:
			((int64 *) data)[row] = DatumGetInt64(d);
			break;
		case DUCKDB_TYPE_FLOAT:
			((float *) data)[row] = DatumGetFloat4(d);
			break;
		case DUCKDB_TYPE_DOUBLE:
			((double *) data)[row] = DatumGetFloat8(d);
			break;
		case DUCKDB_TYPE_DECIMAL:
			{
				char	   *s = DatumGetCString(DirectFunctionCall1(numeric_out, d));
				int128		v = numeric_text_to_scaled(s, ti->scale, ti->width);

				if (ti->width <= 4)
					((int16 *) data)[row] = (int16) v;
				else if (ti->width <= 9)
					((int32 *) data)[row] = (int32) v;
				else if (ti->width <= 18)
					((int64 *) data)[row] = (int64) v;
				else
				{
					duckdb_hugeint *h = &((duckdb_hugeint *) data)[row];

					h->lower = (uint64) (v & 0xffffffffffffffffULL);
					h->upper = (int64) (v >> 64);
				}
				break;
			}
		case DUCKDB_TYPE_STRUCT:
			{
				/* a numeric aggregate state: sum and count children */
				duckdb_hugeint *h = &((duckdb_hugeint *) t->sum_data)[row];
				bool		sum_null;
				int128		sum;
				int64		n;

				numeric_state_unpack(d, ti->scale, &sum_null, &sum, &n);
				validity_set(t->sum_validity, row, !sum_null);
				if (!sum_null)
				{
					h->lower = (uint64) (sum & 0xffffffffffffffffULL);
					h->upper = (int64) (sum >> 64);
				}
				((int64 *) t->count_data)[row] = n;
				break;
			}
		case DUCKDB_TYPE_VARCHAR:
		case DUCKDB_TYPE_BLOB:
			{
				struct varlena *v = PG_DETOAST_DATUM_PACKED(d);
				const char *p = VARDATA_ANY(v);
				int			l = VARSIZE_ANY_EXHDR(v);

				/* bpchar: the blank padding is not part of the value */
				if (ti->typid == BPCHAROID)
					l = bpchartruelen((char *) p, l);
				*str = p;
				*len = l;
				break;
			}
		case DUCKDB_TYPE_DATE:
			{
				DateADT		dt = DatumGetDateADT(d);
				int32		out;

				if (dt == DATEVAL_NOEND)
					out = DUCK_DATE_INF;
				else if (dt == DATEVAL_NOBEGIN)
					out = DUCK_DATE_NINF;
				else
					out = dt + GG_EPOCH_DAYS;
				((int32 *) data)[row] = out;
				break;
			}
		case DUCKDB_TYPE_TIMESTAMP:
		case DUCKDB_TYPE_TIMESTAMP_TZ:
			{
				Timestamp	ts = DatumGetTimestamp(d);
				int64		out;

				if (ts == DT_NOEND)
					out = DUCK_TS_INF;
				else if (ts == DT_NOBEGIN)
					out = DUCK_TS_NINF;
				else
					out = ts + GG_EPOCH_USECS;
				((int64 *) data)[row] = out;
				break;
			}
		case DUCKDB_TYPE_INTERVAL:
			{
				Interval   *iv = DatumGetIntervalP(d);
				duckdb_interval *out = &((duckdb_interval *) data)[row];

				out->months = iv->month;
				out->days = iv->day;
				out->micros = iv->time;
				break;
			}
		case DUCKDB_TYPE_UUID:
			{
				pg_uuid_t  *u = DatumGetUUIDP(d);
				duckdb_hugeint *h = &((duckdb_hugeint *) data)[row];
				uint64		hi = 0,
							lo = 0;
				int			i;

				for (i = 0; i < 8; i++)
					hi = (hi << 8) | u->data[i];
				for (i = 8; i < 16; i++)
					lo = (lo << 8) | u->data[i];
				/* DuckDB flips the top bit so that UUID order equals text order */
				h->upper = (int64) (hi ^ (UINT64CONST(1) << 63));
				h->lower = lo;
				break;
			}
		default:
			elog(ERROR, "gg_duckdb: no writer for DuckDB type %d", (int) ti->duck);
	}
}

/* ---------- reading row `row` of a DuckDB vector into a Datum ---------- */

/*
 * The vector's DuckDB type is `vt` (with `width`/`scale` for DECIMAL), which
 * the caller verified against the expected PG type.  Palloc'd results land
 * in CurrentMemoryContext.
 */
Datum
gg_duckdb_read_datum(const GGTypeInfo *ti, duckdb_type vt, int width, int scale,
					 duckdb_vector vec, void *data, uint64_t *validity, idx_t row,
					 bool *isnull)
{
	if (validity && !duckdb_validity_row_is_valid(validity, row))
	{
		*isnull = true;
		return (Datum) 0;
	}
	*isnull = false;

	if (vt == DUCKDB_TYPE_STRUCT && gg_duckdb_is_numeric_state(ti))
	{
		duckdb_vector sv = duckdb_struct_vector_get_child(vec, 0);
		duckdb_vector nv = duckdb_struct_vector_get_child(vec, 1);
		uint64_t   *sval = duckdb_vector_get_validity(sv);
		bool		sum_null = sval && !duckdb_validity_row_is_valid(sval, row);
		int128		sum = 0;
		int64		n = ((int64 *) duckdb_vector_get_data(nv))[row];

		if (!sum_null)
		{
			duckdb_hugeint *h = &((duckdb_hugeint *) duckdb_vector_get_data(sv))[row];

			sum = ((int128) h->upper << 64) | (int128) h->lower;
		}
		return numeric_state_pack(sum_null, sum, n, ti->scale);
	}

	switch (ti->typid)
	{
		case BOOLOID:
			return BoolGetDatum(((bool *) data)[row]);
		case INT2OID:
			return Int16GetDatum(((int16 *) data)[row]);
		case INT4OID:
			return Int32GetDatum(((int32 *) data)[row]);
		case INT8OID:
			return Int64GetDatum(((int64 *) data)[row]);
		case FLOAT4OID:
			return Float4GetDatum(((float *) data)[row]);
		case FLOAT8OID:
			return Float8GetDatum(((double *) data)[row]);
		case NUMERICOID:
			{
				int128		v;
				char	   *s;

				if (width <= 4)
					v = ((int16 *) data)[row];
				else if (width <= 9)
					v = ((int32 *) data)[row];
				else if (width <= 18)
					v = ((int64 *) data)[row];
				else
				{
					duckdb_hugeint *h = &((duckdb_hugeint *) data)[row];

					v = ((int128) h->upper << 64) | (int128) h->lower;
				}
				s = scaled_to_numeric_text(v, scale);
				return DirectFunctionCall3(numeric_in, CStringGetDatum(s),
										   ObjectIdGetDatum(InvalidOid),
										   Int32GetDatum(ti->typmod));
			}
		case TEXTOID:
		case VARCHAROID:
		case BYTEAOID:
			{
				duckdb_string_t *s = &((duckdb_string_t *) data)[row];
				uint32		len = duckdb_string_t_length(*s);
				const char *p = duckdb_string_t_data(s);

				return PointerGetDatum(cstring_to_text_with_len(p, len));
			}
		case BPCHAROID:
			{
				duckdb_string_t *s = &((duckdb_string_t *) data)[row];
				uint32		len = duckdb_string_t_length(*s);
				const char *p = duckdb_string_t_data(s);
				char	   *cstr = pnstrdup(p, len);

				/* bpcharin re-pads to the declared length */
				return DirectFunctionCall3(bpcharin, CStringGetDatum(cstr),
										   ObjectIdGetDatum(InvalidOid),
										   Int32GetDatum(ti->typmod));
			}
		case DATEOID:
			{
				int32		in = ((int32 *) data)[row];
				DateADT		out;

				if (in >= DUCK_DATE_INF)
					out = DATEVAL_NOEND;
				else if (in <= DUCK_DATE_NINF)
					out = DATEVAL_NOBEGIN;
				else
					out = in - GG_EPOCH_DAYS;
				return DateADTGetDatum(out);
			}
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			{
				int64		in = ((int64 *) data)[row];
				Timestamp	out;

				if (in >= DUCK_TS_INF)
					out = DT_NOEND;
				else if (in <= DUCK_TS_NINF)
					out = DT_NOBEGIN;
				else
					out = in - GG_EPOCH_USECS;
				return TimestampGetDatum(out);
			}
		case INTERVALOID:
			{
				duckdb_interval *in = &((duckdb_interval *) data)[row];
				Interval   *out = (Interval *) palloc(sizeof(Interval));

				out->month = in->months;
				out->day = in->days;
				out->time = in->micros;
				return IntervalPGetDatum(out);
			}
		case UUIDOID:
			{
				duckdb_hugeint *h = &((duckdb_hugeint *) data)[row];
				pg_uuid_t  *u = (pg_uuid_t *) palloc(sizeof(pg_uuid_t));
				uint64		hi = ((uint64) h->upper) ^ (UINT64CONST(1) << 63);
				uint64		lo = h->lower;
				int			i;

				for (i = 7; i >= 0; i--)
				{
					u->data[i] = (unsigned char) (hi & 0xff);
					hi >>= 8;
				}
				for (i = 15; i >= 8; i--)
				{
					u->data[i] = (unsigned char) (lo & 0xff);
					lo >>= 8;
				}
				return UUIDPGetDatum(u);
			}
		default:
			elog(ERROR, "gg_duckdb: no reader for type %u", ti->typid);
	}
	return (Datum) 0;			/* keep compiler quiet */
}
