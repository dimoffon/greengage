-- Local: co-located join of orders and lineitem with a grouped aggregate
SELECT o_orderpriority, count(*) AS lines, sum(l_quantity) AS qty,
       sum(l_extendedprice * (1 - l_discount)) AS revenue, max(l_shipdate) AS last_ship
FROM orders JOIN lineitem ON o_orderkey = l_orderkey
WHERE o_orderdate >= date '1996-01-01'
GROUP BY o_orderpriority
ORDER BY o_orderpriority;
