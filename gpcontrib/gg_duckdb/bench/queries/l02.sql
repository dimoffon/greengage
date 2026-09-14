-- Local: top orders by revenue (sort and limit over a co-located join)
SELECT l_orderkey, o_custkey, sum(l_extendedprice * (1 - l_discount)) AS revenue
FROM orders JOIN lineitem ON o_orderkey = l_orderkey
WHERE o_orderdate >= date '1997-01-01' AND o_orderdate < date '1997-04-01'
GROUP BY l_orderkey, o_custkey
ORDER BY revenue DESC, l_orderkey
LIMIT 50;
