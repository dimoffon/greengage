"""Fill MinIO with test files and create an Iceberg table through the REST catalog.

The rows follow the same formulas as gpcontrib/gg_duckdb/test/external/run.sh
generates in PostgreSQL, so the checks compare against a reference.
"""
import io
import os
import time
from decimal import Decimal

import boto3
import pyarrow as pa
import pyarrow.parquet as pq
import pyarrow.csv as pcsv
from botocore.client import Config

S3_ENDPOINT = os.environ["S3_ENDPOINT"]
REST = os.environ["REST_ENDPOINT"]
KEY = os.environ["AWS_ACCESS_KEY_ID"]
SECRET = os.environ["AWS_SECRET_ACCESS_KEY"]
BUCKET = "ggduck"

s3 = boto3.client("s3", endpoint_url=S3_ENDPOINT, aws_access_key_id=KEY,
                  aws_secret_access_key=SECRET, region_name="us-east-1",
                  config=Config(s3={"addressing_style": "path"}))
try:
    s3.create_bucket(Bucket=BUCKET)
except s3.exceptions.BucketAlreadyOwnedByYou:
    pass


def events(lo, hi, step, offset):
    ids = list(range(lo + offset, hi, step))
    return pa.table({
        "id": pa.array(ids, pa.int64()),
        "k": pa.array([i % 7 for i in ids], pa.int64()),
        "v": pa.array(["v%d" % i for i in ids], pa.string()),
        "amt": pa.array([Decimal(i) * Decimal("1.25") for i in ids], pa.decimal128(12, 2)),
        "ts": pa.array([(1704067200 + (i % 100) * 86400) * 1_000_000 for i in ids], pa.timestamp("us")),
        "flag": pa.array([i % 3 == 0 for i in ids], pa.bool_()),
        "nullable": pa.array([None if i % 13 == 0 else i for i in ids], pa.int64()),
    })


def put(key, data):
    s3.put_object(Bucket=BUCKET, Key=key, Body=data)
    print("wrote s3://%s/%s (%d bytes)" % (BUCKET, key, len(data)))


# six Parquet parts of one table under files/events/
for part in range(6):
    buf = io.BytesIO()
    pq.write_table(events(0, 6000, 6, part), buf)
    put("files/events/part-%d.parquet" % part, buf.getvalue())

# a CSV and a JSON file
items = pa.table({"id": pa.array(range(50), pa.int64()),
                  "name": pa.array(["c%d" % i for i in range(50)]),
                  "price": pa.array([Decimal(i) * 2 for i in range(50)], pa.decimal128(8, 2))})
buf = io.BytesIO()
pcsv.write_csv(items, buf)
put("files/items.csv", buf.getvalue())
put("files/tags.json", "\n".join('{"id": %d, "tag": "j%d"}' % (i, i) for i in range(20)).encode())

# an Iceberg table through the REST catalog: two appends and a delete, so the
# current snapshot spans several data files and is not the first one
from pyiceberg.catalog import load_catalog

for attempt in range(30):
    try:
        catalog = load_catalog("rest", **{
            "type": "rest",
            "uri": REST,
            "s3.endpoint": S3_ENDPOINT,
            "s3.access-key-id": KEY,
            "s3.secret-access-key": SECRET,
            "s3.region": "us-east-1",
            "s3.path-style-access": "true",
        })
        catalog.list_namespaces()
        break
    except Exception as e:  # the catalog starts slower than MinIO
        print("waiting for the REST catalog:", e)
        time.sleep(5)
else:
    raise SystemExit("the REST catalog did not come up")

catalog.create_namespace_if_not_exists("demo")
if ("demo", "events") in catalog.list_tables("demo"):
    catalog.drop_table("demo.events")
first = events(0, 3000, 1, 0)
table = catalog.create_table("demo.events", schema=first.schema)
table.append(first)
table.append(events(3000, 6000, 1, 0))
table.delete("id < 100")
print("iceberg table demo.events:", table.metadata_location)
print("snapshots:", len(table.metadata.snapshots))
print("done")
