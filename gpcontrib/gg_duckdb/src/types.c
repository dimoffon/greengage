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

/* ---------- numeric <-> DECIMAL, directly on the base-10000 digits ---------- */

/*
 * numeric.c keeps its storage format to itself; this is that format, stable
 * since PostgreSQL 9.1: a varlena whose payload is a header word (sign,
 * display scale and, in the short form, the weight) followed by int16
 * digits in base 10000, the first digit weighing 10000^weight.  The long
 * form keeps the weight in a word of its own.  Values in heap tuples are
 * short varlenas, so the payload may be unaligned: every field is read
 * with memcpy.  A self-test on first use converts known values both ways
 * against numeric_in()/numeric_out(), so a layout change fails loudly
 * instead of corrupting values.  Going through the text forms instead
 * cost 75 ns per value in and 380 ns out; this costs a few nanoseconds.
 */
#define GG_NUMERIC_SIGN_MASK			0xC000
#define GG_NUMERIC_POS					0x0000
#define GG_NUMERIC_NEG					0x4000
#define GG_NUMERIC_SHORT				0x8000
#define GG_NUMERIC_NAN					0xC000
#define GG_NUMERIC_SHORT_SIGN_MASK		0x2000
#define GG_NUMERIC_SHORT_DSCALE_MASK	0x1F80
#define GG_NUMERIC_SHORT_DSCALE_SHIFT	7
#define GG_NUMERIC_SHORT_WEIGHT_SIGN_MASK 0x0040
#define GG_NUMERIC_SHORT_WEIGHT_MASK	0x003F
#define GG_NUMERIC_DSCALE_MASK			0x3FFF
#define GG_NBASE						10000
#define GG_DEC_DIGITS					4
#define GG_NUMERIC_MAX_DIGITS			40	/* base-10000 digits of a DECIMAL(38), with room */

typedef unsigned __int128 uint128_t;

typedef struct GGNumericView
{
	bool		neg;
	int			weight;			/* of the first digit, in base 10000 */
	int			dscale;
	int			ndigits;
	const char *digits;			/* int16s, possibly unaligned */
} GGNumericView;

static bool numeric_ready = false;
static uint128_t pow10_128[39];

static void numeric_init(void);

static inline int16
read_int16(const char *p)
{
	int16		v;

	memcpy(&v, p, sizeof(v));
	return v;
}

/* Decode a numeric Datum, in any varlena form, without copying it. */
static void
numeric_view(Datum d, GGNumericView *nv)
{
	struct varlena *v = PG_DETOAST_DATUM_PACKED(d);
	const char *p = VARDATA_ANY(v);
	int			len = VARSIZE_ANY_EXHDR(v);
	uint16		header;

	if (!numeric_ready)
		numeric_init();
	memcpy(&header, p, sizeof(header));
	if ((header & GG_NUMERIC_SIGN_MASK) == GG_NUMERIC_NAN)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("gg_duckdb: numeric value NaN cannot be handed to DuckDB")));
	if ((header & GG_NUMERIC_SIGN_MASK) == GG_NUMERIC_SHORT)
	{
		nv->neg = (header & GG_NUMERIC_SHORT_SIGN_MASK) != 0;
		nv->dscale = (header & GG_NUMERIC_SHORT_DSCALE_MASK) >> GG_NUMERIC_SHORT_DSCALE_SHIFT;
		nv->weight = header & GG_NUMERIC_SHORT_WEIGHT_MASK;
		if (header & GG_NUMERIC_SHORT_WEIGHT_SIGN_MASK)
			nv->weight -= GG_NUMERIC_SHORT_WEIGHT_MASK + 1;
		nv->digits = p + sizeof(uint16);
		nv->ndigits = (len - sizeof(uint16)) / sizeof(int16);
	}
	else
	{
		nv->neg = (header & GG_NUMERIC_SIGN_MASK) == GG_NUMERIC_NEG;
		nv->dscale = header & GG_NUMERIC_DSCALE_MASK;
		nv->weight = read_int16(p + sizeof(uint16));
		nv->digits = p + 2 * sizeof(uint16);
		nv->ndigits = (len - 2 * sizeof(uint16)) / sizeof(int16);
	}
}

static void
numeric_does_not_fit(const GGNumericView *nv, int width, int scale) pg_attribute_noreturn();

static void
numeric_does_not_fit(const GGNumericView *nv, int width, int scale)
{
	ereport(ERROR,
			(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
			 errmsg("gg_duckdb: a numeric value with %d digits at weight %d does not fit DECIMAL(%d,%d)",
					nv->ndigits * GG_DEC_DIGITS, nv->weight, width, scale)));
}

/*
 * The view's value times 10^scale as a 128-bit integer, for DECIMAL(w,s).
 * The last digit may carry decimal digits beyond the scale (the fraction is
 * stored in groups of four); they must be zero, as they are for a value of a
 * numeric(p,s) column, and are dropped before they can overflow the
 * accumulator.  A value that does not fit the width is an error.
 */
static int128
numeric_view_to_scaled(const GGNumericView *nv, int scale, int width)
{
	uint128_t	v = 0;
	int			ndig = nv->ndigits;
	int			drop;			/* decimal digits of the last base-10000 digit beyond the scale */
	int			pad = 0;		/* decimal digits the scale needs beyond the stored ones */
	int			last;
	int			i;

	if (ndig == 0)
		return 0;
	drop = GG_DEC_DIGITS * (ndig - 1 - nv->weight) - scale;
	while (drop >= GG_DEC_DIGITS)
	{
		if (read_int16(nv->digits + (ndig - 1) * sizeof(int16)) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("gg_duckdb: a numeric value has more than %d fractional digits", scale)));
		ndig--;
		drop -= GG_DEC_DIGITS;
		if (ndig == 0)
			return 0;
	}
	if (drop < 0)
	{
		pad = -drop;
		drop = 0;
	}
	last = (uint16) read_int16(nv->digits + (ndig - 1) * sizeof(int16));
	if (drop > 0)
	{
		int			p = (int) pow10_128[drop];

		if (last % p != 0)
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("gg_duckdb: a numeric value has more than %d fractional digits", scale)));
		last /= p;
	}
	for (i = 0; i < ndig - 1; i++)
	{
		/* v * 10000 + 9999 stays below 10^38 while v <= 10^34 */
		if (v > pow10_128[34])
			numeric_does_not_fit(nv, width, scale);
		v = v * GG_NBASE + (uint16) read_int16(nv->digits + i * sizeof(int16));
	}
	/* the last digit contributes 4 - drop decimal digits */
	if (v > pow10_128[34 + drop])
		numeric_does_not_fit(nv, width, scale);
	v = v * (uint128_t) pow10_128[GG_DEC_DIGITS - drop] + (uint128_t) last;
	for (; pad > 0; pad--)
	{
		if (v > pow10_128[37])
			numeric_does_not_fit(nv, width, scale);
		v *= 10;
	}
	if (v >= pow10_128[width])
		numeric_does_not_fit(nv, width, scale);
	return nv->neg ? -(int128) v : (int128) v;
}

/*
 * The base-10000 digits of a / 10^scale as make_result() stores them: no
 * leading or trailing zero digits, weight of the first; 0 digits for zero.
 */
static int
scaled_to_digits(uint128_t a, int scale, int16 *digits, int *weight)
{
	int			fgroups = (scale + GG_DEC_DIGITS - 1) / GG_DEC_DIGITS;
	int			pad = fgroups * GG_DEC_DIGITS - scale;
	uint128_t	ip = a / pow10_128[scale];
	uint128_t	fp = (a % pow10_128[scale]) * pow10_128[pad];
	int16		buf[GG_NUMERIC_MAX_DIGITS];
	int			n = 0;
	int			first,
				last,
				i;

	/* least significant first: the padded fraction, then the integer part */
	for (i = 0; i < fgroups; i++)
	{
		buf[n++] = (int16) (fp % GG_NBASE);
		fp /= GG_NBASE;
	}
	do
	{
		buf[n++] = (int16) (ip % GG_NBASE);
		ip /= GG_NBASE;
	} while (ip != 0);

	first = n - 1;
	while (first >= 0 && buf[first] == 0)
		first--;
	last = 0;
	while (last <= first && buf[last] == 0)
		last++;
	if (first < last)
	{
		*weight = 0;
		return 0;
	}
	/* the most significant integer digit weighs n - fgroups - 1; stripped leading zeros lower it */
	*weight = (n - fgroups - 1) - ((n - 1) - first);
	for (i = 0; i <= first - last; i++)
		digits[i] = buf[first - i];
	return first - last + 1;
}

/* A Numeric Datum for v / 10^scale, with scale fractional digits shown. */
static Datum
scaled_to_numeric(int128 v, int scale)
{
	int16		digits[GG_NUMERIC_MAX_DIGITS];
	bool		neg = v < 0;
	uint128_t	a = neg ? -(uint128_t) v : (uint128_t) v;
	int			weight;
	int			n;
	uint16		header;
	Size		len;
	char	   *res;

	if (!numeric_ready)
		numeric_init();
	n = scaled_to_digits(a, scale, digits, &weight);
	if (n == 0)
		neg = false;
	/* dscale <= 38 and |weight| <= 10: always the short form */
	header = GG_NUMERIC_SHORT |
		(neg ? GG_NUMERIC_SHORT_SIGN_MASK : 0) |
		(scale << GG_NUMERIC_SHORT_DSCALE_SHIFT) |
		(weight < 0 ? GG_NUMERIC_SHORT_WEIGHT_SIGN_MASK : 0) |
		(weight & GG_NUMERIC_SHORT_WEIGHT_MASK);
	len = VARHDRSZ + sizeof(uint16) + n * sizeof(int16);
	res = palloc(len);
	SET_VARSIZE(res, len);
	memcpy(res + VARHDRSZ, &header, sizeof(header));
	if (n > 0)
		memcpy(res + VARHDRSZ + sizeof(uint16), digits, n * sizeof(int16));
	return PointerGetDatum(res);
}

/*
 * First use: the powers of ten, then known values both ways against the
 * server's own numeric_in()/numeric_out(), including a hand-built value
 * in the long form (which make_result() never produces for these widths).
 */
static void
numeric_init(void)
{
	static const struct
	{
		const char *in;
		int			scale;
		const char *out;		/* how scaled_to_numeric() of the expected value prints */
		int			ndigits;	/* decimal digits of |expected|, for building it */
	}			cases[] =
	{
		{"0", 2, "0.00", 0},
		{"1", 0, "1", 1},
		{"-1", 0, "-1", 1},
		{"123.45", 2, "123.45", 5},
		{"0.05", 2, "0.05", 1},
		{"-0.0005", 4, "-0.0005", 1},
		{"1.10", 2, "1.10", 3},
		{"5000000", 2, "5000000.00", 9},
		{"12345.6789", 4, "12345.6789", 9},
		{"99999999999999999999999999999999999999", 0, "99999999999999999999999999999999999999", 38},
		{"-9999999999999999.99", 2, "-9999999999999999.99", 18},
		{"100000000000000000000.000001", 6, "100000000000000000000.000001", 27},
		{"1.5", 2, "1.50", 3},
		{"0.1", 3, "0.100", 3},
		{"-1234567890123456789012345678.0123456789", 10, "-1234567890123456789012345678.0123456789", 38},
		{"9999999999999999999999999999.9999999999", 10, "9999999999999999999999999999.9999999999", 38},
		{"0.0000000001", 10, "0.0000000001", 1},
	};
	int			i;

	pow10_128[0] = 1;
	for (i = 1; i < 39; i++)
		pow10_128[i] = pow10_128[i - 1] * 10;
	numeric_ready = true;

	for (i = 0; i < (int) lengthof(cases); i++)
	{
		Datum		d = DirectFunctionCall3(numeric_in, CStringGetDatum(cases[i].in),
											ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
		GGNumericView nv;
		int128		expect = 0;
		int128		got;
		const char *p;
		char	   *back;

		/* the expected scaled value from the text itself */
		{
			int			frac = -1;

			for (p = cases[i].in; *p; p++)
			{
				if (*p == '.')
					frac = 0;
				else if (*p >= '0' && *p <= '9')
				{
					expect = expect * 10 + (*p - '0');
					if (frac >= 0)
						frac++;
				}
			}
			for (frac = Max(frac, 0); frac < cases[i].scale; frac++)
				expect *= 10;
			if (cases[i].in[0] == '-')
				expect = -expect;
		}
		numeric_view(d, &nv);
		got = numeric_view_to_scaled(&nv, cases[i].scale, 38);
		back = DatumGetCString(DirectFunctionCall1(numeric_out, scaled_to_numeric(expect, cases[i].scale)));
		if (got != expect || strcmp(back, cases[i].out) != 0)
		{
			numeric_ready = false;
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("gg_duckdb: the numeric storage layout is not the one this build expects"),
					 errdetail("%s with scale %d read back as %s.", cases[i].in, cases[i].scale, back)));
		}
	}

	{
		/* 123.45 in the long form: sign+dscale word, weight word, digits 123 and 4500 */
		char		buf[VARHDRSZ + 4 + 4];
		uint16		sd = GG_NUMERIC_POS | 2;
		int16		w = 0;
		int16		dg[2] = {123, 4500};
		GGNumericView nv;

		SET_VARSIZE(buf, sizeof(buf));
		memcpy(buf + VARHDRSZ, &sd, 2);
		memcpy(buf + VARHDRSZ + 2, &w, 2);
		memcpy(buf + VARHDRSZ + 4, dg, 4);
		numeric_view(PointerGetDatum(buf), &nv);
		if (numeric_view_to_scaled(&nv, 2, 38) != 12345)
		{
			numeric_ready = false;
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("gg_duckdb: the numeric storage layout is not the one this build expects"),
					 errdetail("The long form of 123.45 did not read back.")));
		}
	}
}

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
	int16		digits[GG_NUMERIC_MAX_DIGITS];
	GGNumericView nv;
	int			ndigits,
				sign,
				i;
	int64		nancount;

	/* read in place: pq_getmsg* only advance the cursor */
	buf.data = VARDATA_ANY(state);
	buf.len = VARSIZE_ANY_EXHDR(state);
	buf.maxlen = buf.len;
	buf.cursor = 0;
	*n = pq_getmsgint64(&buf);
	/* sumX as numeric_send() writes it: ndigits, weight, sign, dscale, digits */
	ndigits = (int16) pq_getmsgint(&buf, 2);
	nv.weight = (int16) pq_getmsgint(&buf, 2);
	sign = pq_getmsgint(&buf, 2);
	nv.dscale = pq_getmsgint(&buf, 2);
	if (sign == GG_NUMERIC_NAN)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("gg_duckdb: a numeric aggregate state holding NaN cannot be handed to DuckDB")));
	if (ndigits < 0 || ndigits > GG_NUMERIC_MAX_DIGITS)
		elog(ERROR, "gg_duckdb: unexpected numeric aggregate state (%d digits)", ndigits);
	for (i = 0; i < ndigits; i++)
		digits[i] = (int16) pq_getmsgint(&buf, 2);
	(void) pq_getmsgint(&buf, 4);	/* maxScale */
	(void) pq_getmsgint64(&buf);	/* maxScaleCount */
	nancount = pq_getmsgint64(&buf);
	if (nancount > 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("gg_duckdb: a numeric aggregate state holding NaN cannot be handed to DuckDB")));
	if (!numeric_ready)
		numeric_init();
	nv.neg = (sign == GG_NUMERIC_NEG);
	nv.ndigits = ndigits;
	nv.digits = (const char *) digits;
	*sum_null = (*n == 0);
	*sum = *sum_null ? 0 : numeric_view_to_scaled(&nv, scale, 38);
}

static Datum
numeric_state_pack(bool sum_null, int128 sum, int64 n, int scale)
{
	StringInfoData buf;
	int16		digits[GG_NUMERIC_MAX_DIGITS];
	bool		neg;
	uint128_t	a;
	int			weight,
				ndigits,
				i;

	if (sum_null)
	{
		n = 0;
		sum = 0;
	}
	if (!numeric_ready)
		numeric_init();
	neg = sum < 0;
	a = neg ? -(uint128_t) sum : (uint128_t) sum;
	ndigits = scaled_to_digits(a, scale, digits, &weight);
	if (ndigits == 0)
		neg = false;
	pq_begintypsend(&buf);
	pq_sendint64(&buf, n);
	pq_sendint16(&buf, ndigits);
	pq_sendint16(&buf, weight);
	pq_sendint16(&buf, neg ? GG_NUMERIC_NEG : GG_NUMERIC_POS);
	pq_sendint16(&buf, scale);
	for (i = 0; i < ndigits; i++)
		pq_sendint16(&buf, digits[i]);
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
				GGNumericView nv;
				int128		v;

				numeric_view(d, &nv);
				v = numeric_view_to_scaled(&nv, ti->scale, ti->width);

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
				return scaled_to_numeric(v, scale);
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
