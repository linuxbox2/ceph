#!/usr/bin/env -S cargo +nightly -Zscript
---
[package]
edition = "2021"
---
//! Runs the FSIO model checker's tiers in parallel.
//!
//! Each run is one invocation of `ceph_test_cls_rgw_fsio_model` with its
//! bounds in the environment;  the runs of a tier are independent, so
//! they run side by side, one core each, and the tier takes as long as
//! its longest run.  Each prints its summary as it ends;  its full
//! output, traces included, goes to `<out>/<name>.log`.
//!
//!   fsio_model_runs.rs [--bin PATH] [--out DIR] [--jobs N (8)] TIER...
//!
//! Tiers:
//!   smoke    small bounds over every configuration kind;  for every change
//!   quick    the checkpoint tier, before a commit
//!   vectors  the vectors in fsio_vectors/ that are not a skipped
//!            check's:  a search from each, in its "# env:" configuration
//!   skips    every skip check, each expected to be caught:  replayed
//!            from its vector (a "# skip:" header) when it has one, else
//!            searched (SHORTEST, STOP_AT_FIRST);  the refinement
//!            expected clean
//!   skips-search  every skip check by search, vector or not
//!
//! A vector whose steps no longer apply fails as STALE VECTOR.  A sample
//! run's violation is also written as <out>/<name>.vector.txt.
//!   sample   random walks (FSIO_MODEL_WALKS) over configurations beyond
//!            the exhaustive search, a seed per run
//!   sample-skips  each skip check under random walks:  walks to catch;
//!            a miss is reported as "missed (sampling)" and does not fail
//!            the run, the exhaustive skips tier being the authority
//!   cluster  the sample tier's configurations, fewer walks, each core
//!            call also sent to a RADOS cluster as its CLS method and
//!            compared (FSIO_MODEL_CLUSTER, fsio_model_cluster.h);  needs
//!            CEPH_CONF and a running vstart, whose keyring is root's:
//!            the tier asks for sudo once and runs the model under it;
//!            and the OSDs' origin check off, since the model sends
//!            every incarnation's calls from one client:
//!            `ceph config set osd rgw_fsio_check_origin false`
//!
//! A run passes when it is clean, or, for a skip check, when it finds a
//! violation;  a run that reaches FSIO_MODEL_LIMIT is reported as a
//! partial sample, not a pass.  The exit status is non-zero if any run
//! fails.

use std::collections::VecDeque;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::{Arc, Mutex};
use std::time::Instant;

const LIMIT: &str = "20000000";

#[derive(Clone)]
struct Run {
    name: String,
    env: Vec<(String, String)>,
    expect_violation: bool,
    /* a measurement of sampling's reach:  a miss is reported, not failed */
    measure: bool,
    /* runs against the cluster, under sudo */
    sudo: bool,
}

fn run(name: &str, env: &[(&str, &str)]) -> Run {
    Run {
        name: name.to_string(),
        env: env.iter().map(|(k, v)| (k.to_string(), v.to_string())).collect(),
        expect_violation: false,
        measure: false,
        sudo: false,
    }
}

fn smoke() -> Vec<Run> {
    vec![
        run("smoke-wr", &[("FSIO_MODEL_ROLES", "wr"), ("FSIO_MODEL_S3_OPS", "0"),
                          ("FSIO_MODEL_ZOMBIES", "0")]),
        run("smoke-w-s3", &[("FSIO_MODEL_ROLES", "w"), ("FSIO_MODEL_S3_OPS", "1")]),
        run("smoke-w+r", &[("FSIO_MODEL_ROLES", "w+r"), ("FSIO_MODEL_S3_OPS", "0")]),
        run("smoke-v-w", &[("FSIO_MODEL_VERSIONED", "1"), ("FSIO_MODEL_ROLES", "w"),
                           ("FSIO_MODEL_S3_OPS", "0"), ("FSIO_MODEL_EXPIRER_CRASHES", "0")]),
        run("smoke-null-w", &[("FSIO_MODEL_VERSIONED", "1"), ("FSIO_MODEL_VSTART", "null"),
                              ("FSIO_MODEL_ROLES", "w"), ("FSIO_MODEL_S3_OPS", "0"),
                              ("FSIO_MODEL_EXPIRER_CRASHES", "0")]),
        run("smoke-v-w+r", &[("FSIO_MODEL_VERSIONED", "1"), ("FSIO_MODEL_ROLES", "w+r"),
                             ("FSIO_MODEL_S3_OPS", "0"), ("FSIO_MODEL_ZOMBIES", "0"),
                             ("FSIO_MODEL_EXPIRER_CRASHES", "0")]),
    ]
}

/* the baselines, without resends (FSIO_MODEL_RESENDS=0), and two runs
 * with them (Z22) */
fn quick() -> Vec<Run> {
    let mut v: Vec<Run> = vec![
        run("quick-wr", &[("FSIO_MODEL_ROLES", "wr"), ("FSIO_MODEL_S3_OPS", "0")]),
        run("quick-v-w", &[("FSIO_MODEL_VERSIONED", "1"), ("FSIO_MODEL_ROLES", "w"),
                           ("FSIO_MODEL_S3_OPS", "0")]),
        run("quick-null-w", &[("FSIO_MODEL_VERSIONED", "1"), ("FSIO_MODEL_VSTART", "null"),
                              ("FSIO_MODEL_ROLES", "w"), ("FSIO_MODEL_S3_OPS", "0")]),
        run("quick-f2", &[("FSIO_MODEL_ROLES", "f"), ("FSIO_MODEL_S3_OPS", "2")]),
        run("quick-w+r", &[("FSIO_MODEL_ROLES", "w+r"), ("FSIO_MODEL_S3_OPS", "0")]),
        run("quick-w+w", &[("FSIO_MODEL_ROLES", "w+w"), ("FSIO_MODEL_S3_OPS", "0")]),
    ];
    for r in &mut v {
        r.env.push(("FSIO_MODEL_RESENDS".into(), "0".into()));
    }
    v.push(run("quick-wr-resend", &[("FSIO_MODEL_ROLES", "wr"), ("FSIO_MODEL_S3_OPS", "0")]));
    v.push(run("quick-v-w-resend", &[("FSIO_MODEL_VERSIONED", "1"), ("FSIO_MODEL_ROLES", "w"),
                                     ("FSIO_MODEL_S3_OPS", "0")]));
    /* W12:  a second read open after the published content moved
     * (FSIO_MODEL_REOPEN, off elsewhere) */
    v.push(run("quick-w+r-reopen", &[("FSIO_MODEL_ROLES", "w+r"), ("FSIO_MODEL_S3_OPS", "0"),
                                     ("FSIO_MODEL_REOPEN", "1"), ("FSIO_MODEL_RESENDS", "0")]));
    /* §3, Z22:  a sequenced call's reply lost (FSIO_MODEL_LOST_REPLY, off
     * elsewhere) */
    v.push(run("quick-wr-lost", &[("FSIO_MODEL_ROLES", "wr"), ("FSIO_MODEL_S3_OPS", "0"),
                                  ("FSIO_MODEL_LOST_REPLY", "1"), ("FSIO_MODEL_RESENDS", "0")]));
    /* §3.3:  size reports and a truncate (FSIO_MODEL_SIZES, off elsewhere) */
    v.push(run("quick-w+w-sizes-lost", &[("FSIO_MODEL_ROLES", "w+w"), ("FSIO_MODEL_S3_OPS", "0"),
                                         ("FSIO_MODEL_SIZES", "1"), ("FSIO_MODEL_LOST_REPLY", "1"),
                                         ("FSIO_MODEL_RESENDS", "0"), ("FSIO_MODEL_ZOMBIES", "0"),
                                         ("FSIO_MODEL_EXPIRER_CRASHES", "0")]));
    v.push(run("quick-w+w-sizes", &[("FSIO_MODEL_ROLES", "w+w"), ("FSIO_MODEL_S3_OPS", "0"),
                                    ("FSIO_MODEL_SIZES", "1"), ("FSIO_MODEL_RESENDS", "0"),
                                    ("FSIO_MODEL_ZOMBIES", "0"),
                                    ("FSIO_MODEL_EXPIRER_CRASHES", "0")]));
    /* §3.6:  a tail-keeping write, a copy onto itself (FSIO_MODEL_KEEP_TAIL,
     * off elsewhere) */
    v.push(run("quick-w+r-keep-tail", &[("FSIO_MODEL_ROLES", "w+r"), ("FSIO_MODEL_S3_OPS", "1"),
                                        ("FSIO_MODEL_KEEP_TAIL", "1"), ("FSIO_MODEL_RESENDS", "0")]));
    v.push(run("quick-v-w+r-reopen", &[("FSIO_MODEL_VERSIONED", "1"), ("FSIO_MODEL_ROLES", "w+r"),
                                       ("FSIO_MODEL_S3_OPS", "0"), ("FSIO_MODEL_REOPEN", "1"),
                                       ("FSIO_MODEL_RESENDS", "0"), ("FSIO_MODEL_ZOMBIES", "0"),
                                       ("FSIO_MODEL_EXPIRER_CRASHES", "0")]));
    v
}

/* a vector's header:  "# skip: CHECK" makes it a skipped check's
 * reproducer, expected caught;  "# env: K=V ..." is the configuration it
 * runs in;  "# replay: walk" replays it as one walk, which a livelock
 * needs (a search stops where the loop revisits a state);  "# replay:
 * search" searches on from a skipped check's prefix rather than
 * replaying it alone, which a state with no end state reachable needs */
struct Vector {
    path: PathBuf,
    skip: Option<String>,
    env: Vec<(String, String)>,
    walk: bool,
    search: bool,
}

fn read_vectors(src: &Path) -> Vec<Vector> {
    let dir = src.join("fsio_vectors");
    let mut names: Vec<PathBuf> = std::fs::read_dir(&dir)
        .map(|d| d.filter_map(|e| e.ok()).map(|e| e.path()).collect())
        .unwrap_or_default();
    names.sort();
    names
        .into_iter()
        .filter(|p| p.extension().map_or(false, |e| e == "txt"))
        .map(|p| {
            let text = std::fs::read_to_string(&p).unwrap_or_default();
            let mut v = Vector { path: p, skip: None, env: Vec::new(), walk: false,
                                 search: false };
            for l in text.lines() {
                if l.trim() == "# replay: walk" {
                    v.walk = true;
                } else if l.trim() == "# replay: search" {
                    v.search = true;
                } else if let Some(c) = l.strip_prefix("# skip: ") {
                    v.skip = Some(c.trim().to_string());
                } else if let Some(e) = l.strip_prefix("# env: ") {
                    for kv in e.split_whitespace() {
                        if let Some((k, val)) = kv.split_once('=') {
                            v.env.push((k.to_string(), val.to_string()));
                        }
                    }
                }
            }
            v
        })
        .collect()
}

fn vector_run(v: &Vector) -> Run {
    let stem = v.path.file_stem().unwrap().to_string_lossy().to_string();
    let mut r = run(&format!("vector-{stem}"), &[]);
    r.env = v.env.clone();
    r.env.push(("FSIO_MODEL_PREFIX".into(), v.path.to_string_lossy().into()));
    if v.walk {
        r.env.push(("FSIO_MODEL_WALKS".into(), "1".into()));
    }
    if let Some(c) = &v.skip {
        r.env.push(("FSIO_MODEL_SKIP_CHECK".into(), c.clone()));
        if !v.walk && !v.search {
            r.env.push(("FSIO_MODEL_PREFIX_ONLY".into(), "1".into()));
        }
        r.env.push(("FSIO_MODEL_STOP_AT_FIRST".into(), "1".into()));
        r.expect_violation = true;
    }
    r
}

/* the vectors that are not a skipped check's:  a search from each */
fn vectors(src: &Path) -> Vec<Run> {
    read_vectors(src).iter().filter(|v| v.skip.is_none()).map(vector_run).collect()
}

/* each check, the configuration that reaches it, and whether skipping it
 * must be caught;  the refinement changes no result.  A check caught
 * only in a narrower configuration carries its own settings, which
 * replace the shared ones:  the narrowest configuration found to catch it
 * within the cap (2026-11-18, after the versioned model grew) */
type Env = &'static [(&'static str, &'static str)];
/* no partitions, no expirer crash, plain depth-first */
const NARROW: Env = &[("FSIO_MODEL_S3_OPS", "0"), ("FSIO_MODEL_ZOMBIES", "0"),
                      ("FSIO_MODEL_EXPIRER_CRASHES", "0"), ("FSIO_MODEL_SHORTEST", "0")];
const NARROW_W_R: Env = &[("FSIO_MODEL_ROLES", "w+r"), ("FSIO_MODEL_S3_OPS", "0"),
                          ("FSIO_MODEL_ZOMBIES", "0"), ("FSIO_MODEL_EXPIRER_CRASHES", "0"),
                          ("FSIO_MODEL_SHORTEST", "0")];

type Check = (&'static str, Env, Env, bool);

fn skip_table() -> Vec<Check> {
    let unv: Env = &[("FSIO_MODEL_ROLES", "wr"), ("FSIO_MODEL_S3_OPS", "1")];
    let ver: Env = &[("FSIO_MODEL_VERSIONED", "1"), ("FSIO_MODEL_ROLES", "wr"),
                     ("FSIO_MODEL_S3_OPS", "1")];
    let null: Env = &[("FSIO_MODEL_VERSIONED", "1"), ("FSIO_MODEL_VSTART", "null"),
                      ("FSIO_MODEL_ROLES", "wr"), ("FSIO_MODEL_S3_OPS", "1")];
    let none: Env = &[];
    vec![
        ("W5_FINALIZE_RECHECKS_STATE", unv, none, true),
        ("W6_EXPIRE_TAKES_OVER_PUBLISH", unv, none, true),
        ("W3_FINALIZE_CREATES_TO_PUBLISHING", unv, none, true),
        ("W3_RESUMED_FINALIZE_TAKES_OVER", unv, none, true),
        ("W3_PUBLISHING_TO_RETIRES_FROM", unv,
         &[("FSIO_MODEL_ROLES", "w"), ("FSIO_MODEL_S3_OPS", "0"), ("FSIO_MODEL_ZOMBIES", "0"),
           ("FSIO_MODEL_EXPIRER_CRASHES", "0"), ("FSIO_MODEL_SHORTEST", "0")], true),
        ("W1_SHADOW_STORES_PSEUDO_MANIFEST", unv, none, true),
        ("CUR_SHADOW_SUPERSEDE", unv, &[("FSIO_MODEL_ROLES", "ww")], true),
        ("CUR_SHADOW_PUBLISH", unv, none, true),
        ("CUR_SHADOW_ABANDON", unv, none, true),
        ("W7_LEAVE_RETURNS_VERSION_READ", null, NARROW_W_R, true),
        ("W7_REBIND_WAITS_ON_PUBLISHING", ver, none, true),
        /* required since I4 (2026-11-19):  a handle learns it is counted
         * only when it gives up a read open, so a counted writer never
         * removes itself */
        ("W7_PUBLISH_COUNTS_READERS_ONLY", ver,
         &[("FSIO_MODEL_ROLES", "w+r"), ("FSIO_MODEL_S3_OPS", "0"), ("FSIO_MODEL_ZOMBIES", "0")],
         true),
        ("V1_FINALIZE_REQUIRES_ABORTED_CLAIM", ver, none, true),
        ("W9_HINT_SPARES_WRITTEN_VERSION", ver, none, true),
        ("W9_HINT_WAITS_FOR_WRITER", ver, none, true),
        ("V2_COMPLETE_REMOVES_DEAD_WRITERS", ver, none, true),
        ("V3_FINALIZER_HINTS_PLACEHOLDER", ver, &[("FSIO_MODEL_ROLES", "w")], true),
        ("V4_HINT_WAITS_FOR_GRACE", ver, none, true),
        ("V4_PUBLISHER_KEEPS_HINT", null,
         &[("FSIO_MODEL_ROLES", "w"), ("FSIO_MODEL_S3_OPS", "0"), ("FSIO_MODEL_CRASHES", "0"),
           ("FSIO_MODEL_EXPIRER_CRASHES", "0"), ("FSIO_MODEL_SHORTEST", "0")], true),
        ("NULL_IS_HEAD", null, none, true),
        ("NULL_REMOVAL_RETIRES", null, none, true),
        ("PUBLISH_FOLLOWS_STATE", null, none, true),
        ("PRESENCE_HINT_BEFORE_JOIN", unv, none, true),
        ("PRESENCE_HINT_BEFORE_OCNT", ver, none, true),
        ("PRESENCE_HINT_HELD_UNTIL_RELEASED", unv, none, true),
        ("H1_FAILED_RELEASE_KEEPS_HINT", unv, none, true),
        ("H2_HINT_DROPPED_AFTER_GRACE", ver,
         &[("FSIO_MODEL_ROLES", "w"), ("FSIO_MODEL_S3_OPS", "0")], true),
        ("H3_PUBLISHER_HINTS_READERS", ver, NARROW_W_R, true),
        /* a gateway that both publishes and later reads its version */
        /* kept, though the hint before every ocnt_add (2026-11-20) now
         * covers its case:  skipped, nothing shows */
        ("I4_PUBLISHER_NOT_COUNTED", ver, &[("FSIO_MODEL_ROLES", "f")], false),
        /* a second read open after the publish its first open followed */
        ("HINT_BEFORE_EVERY_OCNT_ADD", ver,
         &[("FSIO_MODEL_ROLES", "w+r"), ("FSIO_MODEL_S3_OPS", "0"), ("FSIO_MODEL_REOPEN", "1"),
           ("FSIO_MODEL_RESENDS", "0"), ("FSIO_MODEL_ZOMBIES", "0"),
           ("FSIO_MODEL_EXPIRER_CRASHES", "0")], true),
        /* a live gateway's open that finds its version deleted */
        ("GONE_KEEPS_HINT_ONLY_IF_HELD", null,
         &[("FSIO_MODEL_ZOMBIES", "0"), ("FSIO_MODEL_EXPIRER_CRASHES", "0")], true),
        ("H3_TRACKING_ONLY_IS_REMOVED", null,
         &[("FSIO_MODEL_ZOMBIES", "0"), ("FSIO_MODEL_EXPIRER_CRASHES", "0")], false),
        ("H3_HINT_WAITS_FOR_LIVE_CLAIM", null, NARROW, true),
        ("H3_PUBLISH_HINT_REMOVES_READER_HINTS", null, NARROW, true),
        ("H3_PUBLISHER_CLEANUP_REMOVES_READER_HINTS", null,
         &[("FSIO_MODEL_PREFIX", "src/test/cls_rgw/fsio_vectors/h3-tracking-only.txt"),
           ("FSIO_MODEL_SHORTEST", "0")], true),
        ("W10_RELEASE_ONLY_WITH_LIVE_CLAIM", ver, none, true),
        ("W10_RELEASE_ABORTS_CLAIM_FIRST", ver, none, true),
        ("W15_CONSTRUCTOR_MAY_ABANDON", unv, none, true),
        /* the model's expirer clears only a dead constructor's shadow;  the
         * unit test detects it */
        ("A29_CLEAR_RECHECKS_CONSTRUCTOR", unv, NARROW, false),
        /* §3.3:  a lost truncate reply's epoch re-read from the head
         * (kept as a vector) */
        ("LOST_TRUNCATE_KEEPS_EPOCH", unv,
         &[("FSIO_MODEL_ROLES", "w+w"), ("FSIO_MODEL_S3_OPS", "0"), ("FSIO_MODEL_SIZES", "1"),
           ("FSIO_MODEL_LOST_REPLY", "1"), ("FSIO_MODEL_RESENDS", "0"),
           ("FSIO_MODEL_ZOMBIES", "0"), ("FSIO_MODEL_EXPIRER_CRASHES", "0")], true),
        /* §3, Z22:  a lost reply's resend dropping a hint a takeover
         * needs (kept as a vector) */
        ("LOST_REPLY_KEEPS_HINT", unv,
         &[("FSIO_MODEL_ROLES", "w+r"), ("FSIO_MODEL_S3_OPS", "0"),
           ("FSIO_MODEL_LOST_REPLY", "1"), ("FSIO_MODEL_RESENDS", "0")], true),
        /* §3.3:  a stale report merged past a truncate;  a rebase that
         * loses a write made after it (kept as a vector) */
        ("HINT_STALE_MERGES_NOTHING", unv,
         &[("FSIO_MODEL_ROLES", "w+w"), ("FSIO_MODEL_S3_OPS", "0"), ("FSIO_MODEL_SIZES", "1"),
           ("FSIO_MODEL_RESENDS", "0"), ("FSIO_MODEL_ZOMBIES", "0"),
           ("FSIO_MODEL_EXPIRER_CRASHES", "0")], true),
        ("REBASE_READS_CHUNKS", unv,
         &[("FSIO_MODEL_ROLES", "w+w"), ("FSIO_MODEL_S3_OPS", "0"), ("FSIO_MODEL_SIZES", "1"),
           ("FSIO_MODEL_RESENDS", "0")], true),
        /* W14:  the model's writers have all left a publishing shadow, so
         * no call reaches it;  the unit and neorados tests cover it */
        ("W14_PUBLISHING_FREEZES_SIZE", unv,
         &[("FSIO_MODEL_ROLES", "w+w"), ("FSIO_MODEL_S3_OPS", "0"), ("FSIO_MODEL_SIZES", "1"),
           ("FSIO_MODEL_RESENDS", "0"), ("FSIO_MODEL_ZOMBIES", "0"),
           ("FSIO_MODEL_EXPIRER_CRASHES", "0")], false),
        /* a reader of a tail two writes share;  kept as a vector */
        ("SHARED_TAIL_AFTER_LAST_READER", unv, &[("FSIO_MODEL_ROLES", "f"), ("FSIO_MODEL_S3_OPS", "2"),
                                                 ("FSIO_MODEL_KEEP_TAIL", "1")], true),
        /* a reader of the head when S3 copies the object onto itself */
        ("A26_KEEP_TAIL_RECORDS_NO_MANIFEST", unv, &[("FSIO_MODEL_ROLES", "r"),
                                                     ("FSIO_MODEL_KEEP_TAIL", "1")], true),
        /* a reader of a version rewritten in place */
        ("A13_REWRITE_DEFERS", ver, &[("FSIO_MODEL_ROLES", "r")], true),
        /* a second read open between a publish's link and its publish
         * on the head (§3.1 step 4);  kept as a vector */
        ("JOIN_BINDS_LINKED_PUBLISH", ver, NARROW, false),
        /* Z22:  resends (FSIO_MODEL_RESENDS, on by default) */
        ("Z22_COUNTED_CALL_CHECKS_SEQ", unv,
         &[("FSIO_MODEL_ROLES", "wr"), ("FSIO_MODEL_S3_OPS", "0")], true),
        ("Z22_EXPIRY_KEEPS_SEQ", unv,
         &[("FSIO_MODEL_ROLES", "wr"), ("FSIO_MODEL_S3_OPS", "0")], true),
        ("Z22_ABANDON_WAITS_FOR_GRACE", unv,
         &[("FSIO_MODEL_ROLES", "wr"), ("FSIO_MODEL_S3_OPS", "0"), ("FSIO_MODEL_SHORTEST", "0")],
         true),
        /* Z6:  the claim written back is caught as left over;  the
         * resolution's ID-tag test only with it, by its vector */
        ("Z6_INSTANCE_WRITE_DROPS_CLAIM", ver,
         &[("FSIO_MODEL_ROLES", "w"), ("FSIO_MODEL_S3_OPS", "0")], true),
        ("Z6_INSTANCE_WRITE_DROPS_CLAIM,Z6_RESOLUTION_TESTS_ID_TAG", ver, none, true),
        /* a cycle drawing new claims:  depth-first finds it */
        ("RESUME_REPLACES_CLAIM", ver,
         &[("FSIO_MODEL_ROLES", "w"), ("FSIO_MODEL_S3_OPS", "0"), ("FSIO_MODEL_SHORTEST", "0")],
         true),
        /* a livelock:  the livelock check is off under SHORTEST */
        ("RESUME_RESOLVES_CLAIM", ver,
         &[("FSIO_MODEL_ROLES", "w"), ("FSIO_MODEL_S3_OPS", "0"), ("FSIO_MODEL_SHORTEST", "0")],
         true),
        ("W20_RELEASE_CHECKS_PUBLISHER", null,
         &[("FSIO_MODEL_ROLES", "ww"), ("FSIO_MODEL_S3_OPS", "0"), ("FSIO_MODEL_CRASHES", "0"),
           ("FSIO_MODEL_EXPIRER_CRASHES", "0")], true),
    ]
}

/* each skip check:  replayed from its vector when it has one (a check
 * whose search is long), else searched */
fn skips(src: &Path) -> Vec<Run> {
    let vecs = read_vectors(src);
    skip_table()
        .iter()
        .flat_map(|(c, ..)| {
            let mine: Vec<Run> = vecs
                .iter()
                .filter(|v| v.skip.as_deref() == Some(*c))
                .map(|v| {
                    let mut r = vector_run(v);
                    let stem = v.path.file_stem().unwrap().to_string_lossy().to_string();
                    r.name = format!("skip-{c} ({stem})");
                    r
                })
                .collect();
            if mine.is_empty() { vec![skip_search(c)] } else { mine }
        })
        .collect()
}

fn skip_search(check: &str) -> Run {
    skips_search().into_iter().find(|r| r.name == format!("skip-{check}")).unwrap()
}

/* each skip check by search, vector or not */
fn skips_search() -> Vec<Run> {
    skip_table()
        .iter()
        .map(|(c, env, own, caught)| {
            let mut r = run(&format!("skip-{c}"), env);
            r.env.push(("FSIO_MODEL_SKIP_CHECK".into(), c.to_string()));
            if *caught {
                r.env.push(("FSIO_MODEL_SHORTEST".into(), "1".into()));
                r.env.push(("FSIO_MODEL_STOP_AT_FIRST".into(), "1".into()));
            }
            /* later settings replace earlier ones of the same name */
            for (k, v) in own.iter() {
                r.env.retain(|(ek, _)| ek != k);
                r.env.push((k.to_string(), v.to_string()));
            }
            r.expect_violation = *caught;
            r
        })
        .collect()
}

const WALKS: &str = "200000";

fn walks(name: &str, env: &[(&str, &str)], seed: u64) -> Run {
    let mut r = run(name, env);
    r.env.push(("FSIO_MODEL_WALKS".into(), WALKS.into()));
    r.env.push(("FSIO_MODEL_SEED".into(), seed.to_string()));
    r
}

/* random walks over configurations beyond the exhaustive search;  a
 * distinct seed per run */
fn sample() -> Vec<Run> {
    let v: &[(&str, &str)] = &[("FSIO_MODEL_VERSIONED", "1")];
    let n: &[(&str, &str)] = &[("FSIO_MODEL_VERSIONED", "1"), ("FSIO_MODEL_VSTART", "null")];
    let configs: Vec<(&str, Vec<(&str, &str)>)> = vec![
        ("sample-fff-2", vec![("FSIO_MODEL_ROLES", "fff"), ("FSIO_MODEL_S3_OPS", "2")]),
        ("sample-wr-2", vec![("FSIO_MODEL_ROLES", "wr"), ("FSIO_MODEL_S3_OPS", "2")]),
        ("sample-w+r+r-1", vec![("FSIO_MODEL_ROLES", "w+r+r"), ("FSIO_MODEL_S3_OPS", "1")]),
        ("sample-w+r,w-1", vec![("FSIO_MODEL_ROLES", "w+rw"), ("FSIO_MODEL_S3_OPS", "1")]),
        ("sample-v-wr-2", [v, &[("FSIO_MODEL_ROLES", "wr"), ("FSIO_MODEL_S3_OPS", "2")]].concat()),
        ("sample-v-w+r-1", [v, &[("FSIO_MODEL_ROLES", "w+r"), ("FSIO_MODEL_S3_OPS", "1")]].concat()),
        ("sample-v-www-1", [v, &[("FSIO_MODEL_ROLES", "www"), ("FSIO_MODEL_S3_OPS", "1")]].concat()),
        ("sample-null-wr-2", [n, &[("FSIO_MODEL_ROLES", "wr"), ("FSIO_MODEL_S3_OPS", "2")]].concat()),
        /* §3.3:  size reports and a truncate (FSIO_MODEL_SIZES) */
        ("sample-w+w-sizes", vec![("FSIO_MODEL_ROLES", "w+w"), ("FSIO_MODEL_S3_OPS", "1"),
                                  ("FSIO_MODEL_SIZES", "1")]),
    ];
    configs
        .iter()
        .enumerate()
        .map(|(i, (name, env))| walks(name, env, i as u64 + 1))
        .collect()
}

/* the sample tier's configurations against the cluster:  each walk's
 * calls go to the OSD, so fewer walks */
const CLUSTER_WALKS: &str = "2000";

fn cluster() -> Vec<Run> {
    sample()
        .into_iter()
        .map(|mut r| {
            r.name = r.name.replacen("sample-", "cluster-", 1);
            for (k, v) in r.env.iter_mut() {
                if k == "FSIO_MODEL_WALKS" {
                    *v = CLUSTER_WALKS.into();
                }
            }
            r.env.push(("FSIO_MODEL_CLUSTER".into(), "1".into()));
            r.sudo = true;
            r
        })
        .collect()
}

/* each skip check under random walks, on its shared configuration and
 * its own roles only (none of the narrowing the search needed):  how
 * many walks each takes to be caught, or that walks miss it */
fn sample_skips() -> Vec<Run> {
    skip_table()
        .iter()
        .filter(|(_, _, _, caught)| *caught)
        .map(|(c, env, own, _)| {
            let mut r = walks(&format!("sample-skip-{c}"), env, 1);
            for (k, v) in own.iter().filter(|(k, _)| *k == "FSIO_MODEL_ROLES") {
                r.env.retain(|(ek, _)| ek != k);
                r.env.push((k.to_string(), v.to_string()));
            }
            r.env.push(("FSIO_MODEL_SKIP_CHECK".into(), c.to_string()));
            r.env.push(("FSIO_MODEL_STOP_AT_FIRST".into(), "1".into()));
            r.expect_violation = true;
            r.measure = true;
            r
        })
        .collect()
}

/* the first violation's trace in a run's log, as a vector:  its
 * configuration in the header, the steps, the violation as a comment */
fn walk_vector(r: &Run, text: &str) -> Option<String> {
    let start = text.find("\n--- ")? + 1;
    let mut lines = text[start..].lines();
    let what = lines.next()?.trim_start_matches("--- ").to_string();
    let walk = text.lines().find(|l| l.contains("] walk ") && l.contains(" of seed "))
        .map(|l| l.trim_start_matches("[fsio-model] ").trim_end_matches(':').to_string())
        .unwrap_or_default();
    let env: Vec<String> = r.env.iter()
        .filter(|(k, _)| !matches!(k.as_str(), "FSIO_MODEL_WALKS" | "FSIO_MODEL_SEED"))
        .map(|(k, v)| format!("{k}={v}"))
        .collect();
    let mut v = format!("# {what}\n# found by {}, {walk};  to keep it, name the\n\
                         # check whose absence it shows (# skip:) and copy it to\n\
                         # fsio_vectors/\n# replay: walk\n# env: {}\n",
                        r.name, env.join(" "));
    for l in lines {
        let Some(step) = l.strip_prefix("    ") else { break };
        if step.starts_with("=> ") {
            v.push_str(&format!("# {step}\n"));
        } else {
            v.push_str(step);
            v.push('\n');
        }
    }
    Some(v)
}

/* the summary line a run ends with:  "N states, M end states, K kinds of
 * violation" */
fn summary(out: &str) -> (Option<u64>, Option<u64>) {
    for line in out.lines().rev() {
        if let Some(i) = line.find(" states") {
            if line.contains("kinds of violation") {
                let states = line[..i].split_whitespace().next()
                    .and_then(|w| w.parse().ok());
                let kinds = line.split(", ").last()
                    .and_then(|t| t.split_whitespace().next())
                    .and_then(|w| w.parse().ok());
                return (states, kinds);
            }
        }
    }
    (None, None)
}

fn main() {
    let mut args: Vec<String> = std::env::args().skip(1).collect();
    /* run from the worktree root:  the paths below are relative to it */
    let here = PathBuf::from("src/test/cls_rgw");
    let mut bin = PathBuf::from("build/bin/ceph_test_cls_rgw_fsio_model");
    let mut out = PathBuf::from("build/fsio-model-runs");
    let mut jobs = 8;               // a modest default;  --jobs N for more
    let mut tiers = Vec::new();
    while !args.is_empty() {
        let a = args.remove(0);
        match a.as_str() {
            "--bin" => bin = PathBuf::from(args.remove(0)),
            "--out" => out = PathBuf::from(args.remove(0)),
            "--jobs" => jobs = args.remove(0).parse().expect("--jobs N"),
            t => tiers.push(t.to_string()),
        }
    }
    if tiers.is_empty() {
        eprintln!("usage:  fsio_model_runs.rs [--bin PATH] [--out DIR] [--jobs N] \
                   smoke|quick|vectors|skips|skips-search|sample|sample-skips|cluster ...");
        std::process::exit(2);
    }
    let mut runs = Vec::new();
    for t in &tiers {
        match t.as_str() {
            "smoke" => runs.extend(smoke()),
            "quick" => runs.extend(quick()),
            "vectors" => runs.extend(vectors(&here)),
            "skips" => runs.extend(skips(&here)),
            "skips-search" => runs.extend(skips_search()),
            "sample" => runs.extend(sample()),
            "sample-skips" => runs.extend(sample_skips()),
            "cluster" => runs.extend(cluster()),
            other => {
                eprintln!("unknown tier {other}");
                std::process::exit(2);
            }
        }
    }
    std::fs::create_dir_all(&out).expect("create the output directory");
    /* the cluster tier's runs take sudo -n:  one password, asked here */
    if runs.iter().any(|r| r.sudo) {
        if std::env::var("CEPH_CONF").is_err() {
            eprintln!("the cluster tier needs CEPH_CONF");
            std::process::exit(2);
        }
        let ok = Command::new("sudo").arg("-v").status().map(|s| s.success());
        if !matches!(ok, Ok(true)) {
            eprintln!("the cluster tier needs sudo");
            std::process::exit(2);
        }
    }
    let n = runs.len();
    println!("{n} runs, {jobs} at a time;  logs in {}", out.display());
    let queue = Arc::new(Mutex::new(runs.into_iter().collect::<VecDeque<_>>()));
    let failed = Arc::new(Mutex::new(Vec::<String>::new()));
    let start = Instant::now();
    let mut workers = Vec::new();
    for _ in 0..jobs.min(n) {
        let queue = Arc::clone(&queue);
        let failed = Arc::clone(&failed);
        let bin = bin.clone();
        let out = out.clone();
        workers.push(std::thread::spawn(move || loop {
            let Some(r) = queue.lock().unwrap().pop_front() else { break };
            let t0 = Instant::now();
            /* a cluster run under sudo, its environment passed by env(1),
             * which sudo would otherwise reset */
            let mut env: Vec<(String, String)> = vec![
                ("FSIO_MODEL_LIMIT".into(), LIMIT.into()),
                ("FSIO_MODEL_PROGRESS".into(), "1000000".into()),
            ];
            env.extend(r.env.iter().cloned());
            let mut cmd = if r.sudo {
                let mut c = Command::new("sudo");
                c.args(["-n", "env"]);
                if let Ok(conf) = std::env::var("CEPH_CONF") {
                    c.arg(format!("CEPH_CONF={conf}"));
                }
                for (k, v) in &env {
                    c.arg(format!("{k}={v}"));
                }
                c.arg(&bin);
                c
            } else {
                let mut c = Command::new(&bin);
                c.envs(env.iter().map(|(k, v)| (k.as_str(), v.as_str())));
                c
            };
            cmd.stdin(Stdio::null());
            /* the log is written as the run goes, so a long run's
             * progress lines can be read while it runs */
            let log = out.join(format!("{}.log", r.name));
            let f = std::fs::File::create(&log).expect("create the log");
            cmd.stdout(f.try_clone().expect("the log")).stderr(f);
            let _ = cmd.status().expect("run the model");
            let text = std::fs::read_to_string(&log).unwrap_or_default();
            let (states, kinds) = summary(&text);
            /* a vector whose steps no longer apply:  the model prints
             * "prefix step N ... is not available" */
            let stale = text.contains("is not available;  the actions are:");
            let capped = states.map_or(false, |s| s.to_string() == LIMIT);
            let found = kinds.map_or(false, |k| k > 0);
            let verdict = match (r.expect_violation, found, capped) {
                (true, true, _) => "caught",
                (true, false, _) if r.measure => "missed (sampling)",
                (true, false, _) => "MISSED",
                (false, true, _) => "VIOLATION",
                (false, false, true) => "partial (cap)",
                (false, false, false) => "clean",
            };
            let verdict = if stale { "STALE VECTOR" } else { verdict };
            /* a walk's violation, saved as a vector:  promoting it to
             * fsio_vectors/ is a copy */
            if found && !r.expect_violation && text.contains("sampled by") {
                if let Some(v) = walk_vector(&r, &text) {
                    let _ = std::fs::write(out.join(format!("{}.vector.txt", r.name)), v);
                }
            }
            if matches!(verdict, "MISSED" | "VIOLATION" | "STALE VECTOR") || states.is_none() {
                failed.lock().unwrap().push(r.name.clone());
            }
            let walked = text.lines().rev()
                .find_map(|l| l.split("sampled by ").nth(1))
                .and_then(|t| t.split_whitespace().next())
                .map(|w| format!("{w} walks"));
            println!("{:<56} {:>18}  {:>7.1}s  {}",
                     r.name,
                     walked.unwrap_or_else(|| format!(
                         "{} states", states.map_or("?".into(), |s| s.to_string()))),
                     t0.elapsed().as_secs_f64(),
                     if states.is_none() { "NO SUMMARY" } else { verdict });
        }));
    }
    for w in workers {
        let _ = w.join();
    }
    let failed = failed.lock().unwrap();
    println!("{} of {n} as expected, {:.1}s", n - failed.len(),
             start.elapsed().as_secs_f64());
    if !failed.is_empty() {
        println!("not as expected:  {}", failed.join(", "));
        std::process::exit(1);
    }
}
