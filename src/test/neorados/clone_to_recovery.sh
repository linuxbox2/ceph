#!/bin/bash
# CLONE_TO under recovery:  do clones survive backfill, does a CLONE_TO
# during backfill leave every replica consistent, and what does recovery
# cost the sharing.  It fills a size-2 pool with source/clone pairs
# (ceph_bench_neorados_clone_to --populate), marks osd.0 out while issuing
# more CLONE_TOs, marks it back in, then checks that every sibling equals
# its source and deep-scrubs the pool.  It prints the OSDs' summed
# BlueStore allocated and stored bytes at each stage.
#
# Run from the build directory, with a user that can read the cluster's
# keyring, against a fresh vstart with three OSDs and a manager:
#   MON=1 OSD=3 RGW=0 MGR=1 MDS=0 ../src/vstart.sh -n -d
#   ../src/test/neorados/clone_to_recovery.sh
# ceph_bench_neorados_clone_to is EXCLUDE_FROM_ALL:  build it by name.
set -u
B=bin
POOL=cloneamp
PAIRS=${PAIRS:-64}
CEPH="$B/ceph"

log() { echo "== $(date +%T) $*"; }

# wait for this pool's PGs only:  with an OSD out, other pools (.mgr) may
# be unable to place all their replicas and never become clean
wait_clean() {
  for i in $(seq 1 600); do
    local notclean
    notclean=$($CEPH pg ls-by-pool $POOL -f json 2>/dev/null \
      | jq '[.pg_stats[] | select(.state != "active+clean")] | length')
    if [ "$notclean" = "0" ]; then
      return 0
    fi
    sleep 2
  done
  echo "TIMEOUT waiting for $POOL to be active+clean";  $CEPH pg stat;  return 1
}

usage() {  # sum of every OSD's bluestore allocated and stored bytes
  local a=0 s=0
  for o in $($CEPH osd ls); do
    local j
    j=$($CEPH tell osd.$o perf dump bluestore 2>/dev/null)
    a=$(( a + $(echo "$j" | jq '.bluestore.allocated') ))
    s=$(( s + $(echo "$j" | jq '.bluestore.stored') ))
  done
  echo "allocated=$a stored=$s"
}

log "pool $POOL, size 2, 8 PGs"
$CEPH osd pool create $POOL 8 8 >/dev/null
$CEPH osd pool set $POOL size 2 >/dev/null
$CEPH osd pool application enable $POOL test >/dev/null
wait_clean || exit 1

log "populate $PAIRS pairs of 4 MiB (source + CLONE_TO sibling)"
$B/ceph_bench_neorados_clone_to --pool $POOL --populate --objects $PAIRS || exit 1
wait_clean || exit 1
log "before recovery:  $(usage)"

log "osd.0 out;  CLONE_TO during the backfill"
$CEPH osd out 0
$B/ceph_bench_neorados_clone_to --pool $POOL --objects 16 || exit 1
wait_clean || exit 1
log "after backfill away from osd.0:  $(usage)"

log "osd.0 in;  backfill back"
$CEPH osd in 0
sleep 2
wait_clean || exit 1
log "after backfill back to osd.0:  $(usage)"

log "content:  every sibling must equal its source"
bad=0
for o in $($B/rados -p $POOL ls | grep '~' | sort); do
  src=${o%%~*}
  a=$($B/rados -p $POOL get "$src" - 2>/dev/null | md5sum | cut -d' ' -f1)
  b=$($B/rados -p $POOL --object-locator "$src" get "$o" - 2>/dev/null | md5sum | cut -d' ' -f1)
  if [ "$a" != "$b" ]; then
    echo "MISMATCH $o";  bad=$((bad + 1))
  fi
done
echo "siblings checked:  $($B/rados -p $POOL ls | grep -c '~'),  mismatches:  $bad"

log "deep scrub every PG of $POOL"
scrub_from=$($CEPH pg ls-by-pool $POOL -f json | jq -r '[.pg_stats[].last_deep_scrub_stamp] | max')
for pg in $($CEPH pg ls-by-pool $POOL -f json | jq -r '.pg_stats[].pgid'); do
  $CEPH pg deep-scrub $pg >/dev/null
done
# a scrub still queued looks active+clean, so wait until every PG's
# last deep scrub is later than the request
sleep 5
wait_clean || exit 1
for i in $(seq 1 300); do
  stale=$($CEPH pg ls-by-pool $POOL -f json \
    | jq --arg t "$scrub_from" '[.pg_stats[] | select(.last_deep_scrub_stamp <= $t)] | length')
  [ "$stale" = "0" ] && break
  sleep 2
done
echo "PGs not deep-scrubbed since the request:  $stale"
echo "inconsistent PGs:  $($B/rados list-inconsistent-pg $POOL)"
log "done"
