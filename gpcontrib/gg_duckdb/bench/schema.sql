-- TPC-H-shaped schema for the gg_duckdb benchmark.  Decimal columns are
-- narrow enough for the products the queries form to stay within 38 digits.
-- :opts carries table options ('' for heap, or e.g.
-- 'WITH (appendonly=true, orientation=column, compresstype=zstd)').
DROP SCHEMA IF EXISTS tpch CASCADE;
CREATE SCHEMA tpch;
SET search_path = tpch;

CREATE TABLE region (
    r_regionkey int NOT NULL,
    r_name char(25) NOT NULL,
    r_comment varchar(152)
) :opts DISTRIBUTED REPLICATED;

CREATE TABLE nation (
    n_nationkey int NOT NULL,
    n_name char(25) NOT NULL,
    n_regionkey int NOT NULL,
    n_comment varchar(152)
) :opts DISTRIBUTED REPLICATED;

CREATE TABLE supplier (
    s_suppkey int NOT NULL,
    s_name char(25) NOT NULL,
    s_address varchar(40) NOT NULL,
    s_nationkey int NOT NULL,
    s_phone char(15) NOT NULL,
    s_acctbal numeric(12,2) NOT NULL,
    s_comment varchar(101) NOT NULL
) :opts DISTRIBUTED BY (s_suppkey);

CREATE TABLE part (
    p_partkey int NOT NULL,
    p_name varchar(55) NOT NULL,
    p_mfgr char(25) NOT NULL,
    p_brand char(10) NOT NULL,
    p_type varchar(25) NOT NULL,
    p_size int NOT NULL,
    p_container char(10) NOT NULL,
    p_retailprice numeric(12,2) NOT NULL,
    p_comment varchar(23) NOT NULL
) :opts DISTRIBUTED BY (p_partkey);

CREATE TABLE partsupp (
    ps_partkey int NOT NULL,
    ps_suppkey int NOT NULL,
    ps_availqty int NOT NULL,
    ps_supplycost numeric(12,2) NOT NULL,
    ps_comment varchar(199) NOT NULL
) :opts DISTRIBUTED BY (ps_partkey);

CREATE TABLE customer (
    c_custkey int NOT NULL,
    c_name varchar(25) NOT NULL,
    c_address varchar(40) NOT NULL,
    c_nationkey int NOT NULL,
    c_phone char(15) NOT NULL,
    c_acctbal numeric(12,2) NOT NULL,
    c_mktsegment char(10) NOT NULL,
    c_comment varchar(117) NOT NULL
) :opts DISTRIBUTED BY (c_custkey);

CREATE TABLE orders (
    o_orderkey bigint NOT NULL,
    o_custkey int NOT NULL,
    o_orderstatus char(1) NOT NULL,
    o_totalprice numeric(12,2) NOT NULL,
    o_orderdate date NOT NULL,
    o_orderpriority char(15) NOT NULL,
    o_clerk char(15) NOT NULL,
    o_shippriority int NOT NULL,
    o_comment varchar(79) NOT NULL
) :opts DISTRIBUTED BY (o_orderkey);

CREATE TABLE lineitem (
    l_orderkey bigint NOT NULL,
    l_partkey int NOT NULL,
    l_suppkey int NOT NULL,
    l_linenumber int NOT NULL,
    l_quantity numeric(10,2) NOT NULL,
    l_extendedprice numeric(12,2) NOT NULL,
    l_discount numeric(4,2) NOT NULL,
    l_tax numeric(4,2) NOT NULL,
    l_returnflag char(1) NOT NULL,
    l_linestatus char(1) NOT NULL,
    l_shipdate date NOT NULL,
    l_commitdate date NOT NULL,
    l_receiptdate date NOT NULL,
    l_shipinstruct char(25) NOT NULL,
    l_shipmode char(10) NOT NULL,
    l_comment varchar(44) NOT NULL
) :opts DISTRIBUTED BY (l_orderkey);
