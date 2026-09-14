-- Deterministic TPC-H-like data, :sf times the TPC-H row counts, generated
-- inside the database (no dbgen needed).  Values follow the spec's shapes
-- and ranges closely enough for the queries' selectivities; they are not
-- dbgen's values.
SET search_path = tpch;
\set S (:sf * 10000)
\set P (:sf * 200000)
\set C (:sf * 150000)
\set O (:sf * 1500000)

CREATE OR REPLACE FUNCTION tpch.rnd(i bigint, salt int) RETURNS int
LANGUAGE sql IMMUTABLE AS $$ SELECT abs(hashint8(i * 1000003 + salt)) $$;

INSERT INTO region VALUES
 (0, 'AFRICA', 'lar deposits. blithely final packages cajole. regular waters are final requests.'),
 (1, 'AMERICA', 'hs use ironic, even requests. s'),
 (2, 'ASIA', 'ges. thinly even pinto beans ca'),
 (3, 'EUROPE', 'ly final courts cajole furiously final excuse'),
 (4, 'MIDDLE EAST', 'uickly special accounts cajole carefully blithely close requests.');

INSERT INTO nation
SELECT i, n, r, 'nation ' || n FROM (VALUES
 (0,'ALGERIA',0),(1,'ARGENTINA',1),(2,'BRAZIL',1),(3,'CANADA',1),(4,'EGYPT',4),
 (5,'ETHIOPIA',0),(6,'FRANCE',3),(7,'GERMANY',3),(8,'INDIA',2),(9,'INDONESIA',2),
 (10,'IRAN',4),(11,'IRAQ',4),(12,'JAPAN',2),(13,'JORDAN',4),(14,'KENYA',0),
 (15,'MOROCCO',0),(16,'MOZAMBIQUE',0),(17,'PERU',1),(18,'CHINA',2),(19,'ROMANIA',3),
 (20,'SAUDI ARABIA',4),(21,'VIETNAM',2),(22,'RUSSIA',3),(23,'UNITED KINGDOM',3),(24,'UNITED STATES',1)
) AS v(i, n, r);

INSERT INTO supplier
SELECT i, 'Supplier#' || lpad(i::text, 9, '0'), md5(i::text || 's'), rnd(i, 1) % 25,
       lpad((rnd(i, 2) % 999999999)::text, 15, '0'),
       ((rnd(i, 3) % 1099999) - 99999) / 100.0, md5(i::text || 'sc') || md5(i::text || 'sd')
FROM generate_series(1, :S) i;

INSERT INTO part
SELECT i,
       (ARRAY['almond','antique','aquamarine','azure','beige','bisque','black','blanched','blue','blush'])[rnd(i, 10) % 10 + 1] || ' ' ||
       (ARRAY['brown','burlywood','burnished','chartreuse','chiffon','chocolate','coral','cornflower','cornsilk','cream'])[rnd(i, 11) % 10 + 1] || ' ' ||
       (ARRAY['cyan','dark','deep','dim','dodger','drab','firebrick','floral','forest','frosted'])[rnd(i, 12) % 10 + 1],
       'Manufacturer#' || (rnd(i, 13) % 5 + 1),
       'Brand#' || (rnd(i, 13) % 5 + 1) || (rnd(i, 14) % 5 + 1),
       (ARRAY['STANDARD','SMALL','MEDIUM','LARGE','ECONOMY','PROMO'])[rnd(i, 15) % 6 + 1] || ' ' ||
       (ARRAY['ANODIZED','BURNISHED','PLATED','POLISHED','BRUSHED'])[rnd(i, 16) % 5 + 1] || ' ' ||
       (ARRAY['TIN','NICKEL','BRASS','STEEL','COPPER'])[rnd(i, 17) % 5 + 1],
       rnd(i, 18) % 50 + 1,
       (ARRAY['SM','LG','MED','JUMBO','WRAP'])[rnd(i, 19) % 5 + 1] || ' ' ||
       (ARRAY['CASE','BOX','BAG','JAR','PKG','PACK','CAN','DRUM'])[rnd(i, 20) % 8 + 1],
       (90000 + ((i / 10) % 20001) + 100 * (i % 1000)) / 100.0,
       left(md5(i::text || 'p'), 23)
FROM generate_series(1, :P) i;

INSERT INTO partsupp
SELECT (i - 1) / 4 + 1,
       (((i - 1) / 4 + 1) + ((i - 1) % 4) * (:S / 4 + (((i - 1) / 4) / :S))) % :S + 1,
       rnd(i, 21) % 9999 + 1, (rnd(i, 22) % 99900 + 100) / 100.0,
       md5(i::text || 'ps') || md5(i::text || 'pt')
FROM generate_series(1, 4 * :P) i;

INSERT INTO customer
SELECT i, 'Customer#' || lpad(i::text, 9, '0'), md5(i::text || 'c'), rnd(i, 30) % 25,
       lpad((rnd(i, 31) % 999999999)::text, 15, '0'),
       ((rnd(i, 32) % 1099999) - 99999) / 100.0,
       (ARRAY['AUTOMOBILE','BUILDING','FURNITURE','MACHINERY','HOUSEHOLD'])[rnd(i, 33) % 5 + 1],
       md5(i::text || 'cc') || md5(i::text || 'cd') ||
       CASE WHEN rnd(i, 34) % 100 = 0 THEN ' special requests ' ELSE '' END
FROM generate_series(1, :C) i;

-- orders: date, status and priority from the key so lineitem can repeat them
CREATE OR REPLACE FUNCTION tpch.odate(k bigint) RETURNS date
LANGUAGE sql IMMUTABLE AS $$ SELECT date '1992-01-01' + (tpch.rnd(k, 40) % 2406) $$;

INSERT INTO orders
SELECT i,
       CASE WHEN (rnd(i, 41) % :C + 1) % 3 = 0 THEN rnd(i, 41) % :C + 2 ELSE rnd(i, 41) % :C + 1 END,
       CASE WHEN odate(i) < date '1995-06-17' THEN 'F' WHEN rnd(i, 42) % 5 = 0 THEN 'P' ELSE 'O' END,
       (rnd(i, 43) % 45000000 + 100000) / 100.0,
       odate(i),
       (ARRAY['1-URGENT','2-HIGH','3-MEDIUM','4-NOT SPECIFIED','5-LOW'])[rnd(i, 44) % 5 + 1],
       'Clerk#' || lpad((rnd(i, 45) % (:sf * 1000) + 1)::text, 9, '0'),
       0,
       md5(i::text || 'oc') || CASE WHEN rnd(i, 46) % 50 = 0 THEN ' special requests ' ELSE '' END
FROM generate_series(1, :O) i;

INSERT INTO lineitem
SELECT (i - 1) / 4 + 1,
       rnd(i, 50) % :P + 1,
       rnd(i, 51) % :S + 1,
       (i - 1) % 4 + 1,
       rnd(i, 52) % 50 + 1,
       (rnd(i, 52) % 50 + 1) * ((90000 + ((rnd(i, 50) % :P + 1) % 20001) + 100 * (rnd(i, 50) % 1000)) / 100.0),
       (rnd(i, 53) % 11) / 100.0,
       (rnd(i, 54) % 9) / 100.0,
       CASE WHEN odate((i - 1) / 4 + 1) + (rnd(i, 55) % 121 + 1) + (rnd(i, 57) % 30 + 1) <= date '1995-06-17'
            THEN (ARRAY['R','A'])[rnd(i, 58) % 2 + 1] ELSE 'N' END,
       CASE WHEN odate((i - 1) / 4 + 1) + (rnd(i, 55) % 121 + 1) > date '1995-06-17' THEN 'O' ELSE 'F' END,
       odate((i - 1) / 4 + 1) + (rnd(i, 55) % 121 + 1),
       odate((i - 1) / 4 + 1) + (rnd(i, 56) % 61 + 30),
       odate((i - 1) / 4 + 1) + (rnd(i, 55) % 121 + 1) + (rnd(i, 57) % 30 + 1),
       (ARRAY['DELIVER IN PERSON','COLLECT COD','NONE','TAKE BACK RETURN'])[rnd(i, 59) % 4 + 1],
       (ARRAY['REG AIR','AIR','RAIL','SHIP','TRUCK','MAIL','FOB'])[rnd(i, 60) % 7 + 1],
       left(md5(i::text || 'l'), 27)
FROM generate_series(1, 4 * :O) i;

ANALYZE region; ANALYZE nation; ANALYZE supplier; ANALYZE part;
ANALYZE partsupp; ANALYZE customer; ANALYZE orders; ANALYZE lineitem;
