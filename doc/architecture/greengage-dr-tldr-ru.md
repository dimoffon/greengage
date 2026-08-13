# Greengage 7 DR read-only реплика — коротко

**Что это.** Второй кластер Greengage, который постоянно находится в archive
recovery и проигрывает WAL production, полученный через архив. Он отдаёт
**read-only** запросы и в любой момент может быть **промоутнут на месте** в
обычный online read-write кластер. Транспорт — только архивирование WAL и
`restore_command`: pgBackRest/WAL-G не требуются.

**Главная идея.** Реплика отдаёт чтения **не «как сейчас доиграно», а как на
момент распределённой точки восстановления** (`gp_create_restore_point`). Точка
берётся под `TwophaseCommitLock` — это единственный срез, на котором ни одна
распределённая транзакция не «расщеплена» между сегментами. Режима свободного
проигрывания нет намеренно: у чтения, взятого во время реплея, нет
межузловой гарантии.

---

## Schema

```
        PRODUCTION (online, read-write)                     DR REPLICA (in recovery, read-only)
 ┌────────────────────────────────────────┐          ┌────────────────────────────────────────┐
 │  coordinator      seg0      seg1  ...  │          │  coordinator      seg0      seg1  ...  │
 │                                        │          │                                        │
 │  archive_mode = on                     │          │  hot_standby = on   ( = DR mode )      │
 │  archive_command  ─────────────────────┼──┐    ┌──┼─────────────────────  restore_command  │
 │                                        │  │    │  │                                        │
 │  gp_create_restore_point('N')          │  │    │  │  gp_pause_on_restore_point_replay='N'  │
 │    -> one WAL record per instance      │  │    │  │    -> each node stops exactly at N     │
 └────────────────────────────────────────┘  │    │  └────────────────────────────────────────┘
                                             v    │
                                   WAL ARCHIVE (shared)
                                   /archive/wal/seg<content>/
                                   /archive/basebackup/seg<content>/

 Every instance replays INDEPENDENTLY: its own WAL stream, its own LSN space.
 The cluster-wide guarantee comes from the restore point, not from LSN closeness.
```

**Stop-and-go: как срез становится видимым**

```
  ggdr switch N   ( = SELECT gg_dr_switch('N') on the coordinator )

   1. arm      every node: gp_pause_on_restore_point_replay = N   (ALTER SYSTEM + reload)
   2. resume   every node: replay runs forward
   3. freeze   each node, on reaching N, freezes its local MVCC image  -> "pending"
   4. wait     the coordinator waits until EVERY node is paused at N
   5. publish  pending -> served, on all nodes at once

  Reads answer as of the SERVED point the whole time:
        before publish ....... as of N-1        (old data, never torn)
        after  publish ....... as of N

  Because publishing is cluster-wide, "replay reached N" and "reads answer as of N"
  are two different facts.  ggdr stat prints both:  serve_at  and  stopped_at.
```

**Промоушен**

```
  ggdr promote  ( = SELECT gg_dr_promote() )
      segments first, then the coordinator:  pg_promote() + resume
      recovery ends exactly after the served point
      DR mode is keyed on "in recovery + hot_standby", so it lifts with recovery
      -> no restart, no GUC to unset.   IRREVERSIBLE.
```

---

## Что поддерживается

- **Сборка реплики одной командой** — `ggdr create-replica`: разворачивает
  per-instance базовые бэкапы (`pg_basebackup --target-gp-dbid`), пишет
  DR-локальную топологию в `$PGDATA/gp_topology` каждого узла, включает
  `hot_standby` + `restore_command` + `standby.signal` и стартует все узлы в
  recovery.
- **Согласованные чтения как-на-точку** — распределённые (dispatch) read-only
  запросы к координатору реплики: reader gangs, координатор и сегменты в
  recovery, ответ строго на опубликованном срезе.
- **Управление реплеем** — `ggdr switch <rp>` (перейти на срез и опубликовать),
  `ggdr switch <rp> --content <c,...>` (переставить только указанные узлы, без
  публикации), `ggdr pause` (немедленная пауза для инспекции).
- **Наблюдаемость** — `ggdr stat`, представления `gg_stat_dr_replica` и
  `gg_stat_dr_replica_summary`: `serve_at`/`served_restore_point` (на что
  отвечают чтения), `stopped_at`/`restore_point` (докуда доигран реплей),
  `rpo_seconds` (возраст обслуживаемого среза).
- **Промоушен на месте** — `ggdr promote` / `gg_dr_promote()`, без рестарта
  кластера; `gp_configuration_history` заменяется одной строкой о промоушене.
- **Запись запрещена жёстко** — `INSERT`/`UPDATE`/`DELETE`/`CREATE TABLE` и
  `SELECT ... FOR UPDATE` отклоняются с DR-специфичной ошибкой.
- **Топология реплики своя** — она лежит в файле, а не в каталоге, поэтому
  изменения `gp_segment_configuration` на production (в том числе failover
  сегмента) прилетают в WAL, проигрываются и **не меняют** то, чем реплика себя
  описывает.

## Ограничения

- **Только точки восстановления.** Реплика не отдаёт «самые свежие» данные:
  чтения возможны лишь на срезе, и до первой достигнутой точки узел вообще
  отказывает в чтении.
- **Публикация — только кластерная.** `switch --content` переставляет
  подмножество узлов и ничего не публикует. Даже если так переставить *все* узлы
  по одному, кластер останется на старом срезе, пока не выполнить обычный
  `ggdr switch <rp>`.
- **Промоушен требует единой точки** на всех узлах; после `ggdr pause` (это не
  согласованный срез) промоушен отклоняется.
- **`gp_role=utility` — не для чтения данных.** В utility-режиме координатор не
  диспатчит, и распределённая таблица выглядит пустой на любом срезе. Для данных
  нужен обычный `psql`; utility — для состояния отдельного узла.
- **RPO — это возраст среза, а не отставание от production.** Реплика не
  общается с production и не знает его текущий LSN; сравниваются часы двух
  хостов.
- **Failover сегмента на production (смена timeline) — известный разрыв
  (сценарий HA-1).** DR-узел этого content'а остаётся на старой линии времени и
  молча ретраит отсутствующий сегмент WAL; лечится пересборкой этого узла.
- **Неравномерно заархивированный WAL при аварии** (сценарий P-10): выбор
  последней общей точки для промоушена — ручная процедура, автоматики нет.
- **`hot_standby = on` на in-cluster зеркале — задокументированный footgun**
  (сценарий H-4): это не поддерживаемый режим.
- **Управление — через `ggdr`, а не через gpMgmt-утилиты.** Соединения `ggdr` с
  узлами сделаны в расчёте на PoC-развёртывание на одном хосте; для multi-host
  нужен доступ по `address` с соответствующим `pg_hba` или запуск по хостам.
- **В проверенной конфигурации у DR-кластера нет своих зеркал и standby-мастера** —
  реплика строится и управляется по primary-инстансам.

---

## Шпаргалка

```sql
-- на production
SELECT gp_create_restore_point('rp_hourly_42');
SELECT gp_switch_wal();                     -- заставить заархивировать WAL всех инстансов
```

```bash
# на DR
ggdr create-replica --topology dr_topology.tsv --basebackup-dir /archive/basebackup \
                    --wal-archive /archive/wal --pause-at rp_hourly_42
ggdr stat                                   # serve_at / stopped_at / RPO
ggdr switch rp_hourly_43                    # перейти на срез и опубликовать его
ggdr promote --at rp_hourly_43 --yes        # необратимо
```

```sql
-- то же самое из SQL, если доступен только координатор
SELECT gg_dr_switch('rp_hourly_43');
SELECT gg_dr_promote();
SELECT * FROM gg_stat_dr_replica_summary;
```

---

Подробности: [greengage-dr-read-replica.md](greengage-dr-read-replica.md) (дизайн),
[adr/0006-dr-read-replica.md](adr/0006-dr-read-replica.md) и
[adr/0007-pluggable-cluster-topology.md](adr/0007-pluggable-cluster-topology.md)
(решения), [greengage-dr-test-scenarios.md](greengage-dr-test-scenarios.md)
(сценарии и их статус).
