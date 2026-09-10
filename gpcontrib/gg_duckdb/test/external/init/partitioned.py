"""A partitioned Iceberg table for the samples: demo.sales, partitioned by
day(sale_date) and region (identity), created empty through the REST catalog."""
import os, time
import pyarrow as pa
from pyiceberg.catalog import load_catalog
from pyiceberg.partitioning import PartitionSpec, PartitionField
from pyiceberg.transforms import DayTransform, IdentityTransform
from pyiceberg.schema import Schema
from pyiceberg.types import NestedField, LongType, DateType, StringType, DecimalType, TimestampType, BooleanType

for attempt in range(60):
    try:
        catalog = load_catalog("rest", **{
            "uri": os.environ["REST_ENDPOINT"],
            "s3.endpoint": os.environ["S3_ENDPOINT"],
            "s3.access-key-id": os.environ["AWS_ACCESS_KEY_ID"],
            "s3.secret-access-key": os.environ["AWS_SECRET_ACCESS_KEY"],
            "s3.region": os.environ["AWS_REGION"],
        })
        catalog.list_namespaces()
        break
    except Exception as e:
        print("waiting for the REST catalog:", e)
        time.sleep(2)
else:
    raise SystemExit("the REST catalog did not come up")

catalog.create_namespace_if_not_exists("demo")
schema = Schema(
    NestedField(1, "id", LongType(), required=False),
    NestedField(2, "sale_date", DateType(), required=False),
    NestedField(3, "region", StringType(), required=False),
    NestedField(4, "product", StringType(), required=False),
    NestedField(5, "qty", LongType(), required=False),
    NestedField(6, "amount", DecimalType(12, 2), required=False),
    NestedField(7, "sold_at", TimestampType(), required=False),
)
spec = PartitionSpec(
    PartitionField(source_id=2, field_id=1000, transform=DayTransform(), name="sale_day"),
    PartitionField(source_id=3, field_id=1001, transform=IdentityTransform(), name="region"),
)
if ("demo", "sales") in catalog.list_tables("demo"):
    catalog.drop_table("demo.sales")
table = catalog.create_table("demo.sales", schema=schema, partition_spec=spec)
print("iceberg table demo.sales:", table.metadata_location, "spec:", table.spec())
