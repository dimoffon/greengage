-- TPC-H Q14: promotion effect (the CASE over numeric stays on the standard
-- executor in this milestone: its result type has no fixed scale)
SELECT 100.00 * sum(CASE WHEN p_type LIKE 'PROMO%' THEN l_extendedprice * (1 - l_discount) ELSE 0 END)
       / sum(l_extendedprice * (1 - l_discount)) AS promo_revenue
FROM lineitem, part
WHERE l_partkey = p_partkey
  AND l_shipdate >= date '1995-09-01'
  AND l_shipdate < date '1995-10-01';
