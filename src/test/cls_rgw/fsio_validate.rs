#!/usr/bin/env -S cargo +nightly -Zscript
---
[package]
edition = "2021"
---
//! Runs a validation of FSIO on RADOS:  the steps named on the command
//! line, each a command you could run by hand, with one sudo password
//! for the whole run.  `--dry-run` prints them.
//!
//!   fsio_validate.rs [--dry-run] [--no-build] [--build DIR] STEP|PRESET...
//!
//! Steps:
//!   cls            the CLS head test and the neorados CLS test, and
//!                  libcls_rgw calling no RGW symbol          (RADOS)
//!   librgw-rados   ceph_test_librgw_file_fsio_put            (RADOS)
//!   harness        ceph_test_rgw_fsio_rados, the driver's harness (RADOS)
//!   cluster-tier   the model's cluster tier, the origin check off (RADOS)
//!   model          the model's in-memory tiers:  quick vectors skips
//!   nsfs-librgw    write2, and nfsns with every group flag    (nsfs)
//! Presets:
//!   driver   cls librgw-rados harness
//!   core     cls cluster-tier model
//!   full     every step
//!
//! Which steps a change needs is the caller's to say:  the driver runs
//! what it is told and decides nothing about what changed.
//!
//! Run it as yourself, from a terminal:  it takes `sudo -v` once and
//! keeps the ticket alive for the run.  A RADOS vstart needs root here,
//! so the RADOS phase runs under `sudo -n`:  a fresh vstart, the fsio
//! feature enabled, its steps, then the cluster stopped and the build
//! directory's run files handed back to you.  Everything else runs as
//! you, under your own descriptor limit:  the build, the nsfs phase
//! (this tree's `rgw-vstart.sh --store nsfs --clean`, its suites, the
//! gateway stopped) and `model`.  The cluster tier's runner is built as
//! you and its binary run under `sudo -n`, since root has neither your
//! toolchain nor your cargo cache.
//!
//! Before a run it builds what the chosen steps run, unless told
//! `--no-build`;  either way it then requires the build's version to be
//! HEAD's.  Each step's output goes to `<build>/fsio-validate/<time>/
//! <step>.log`, and a line per step to `summary.txt` beside them;  the
//! exit status is non-zero if any step failed.

use std::io::IsTerminal;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

const AWS_KEY: &str = "0555b35654ad1656d804";
const AWS_SECRET: &str = "h7GhxuBLTrlhVUyxSPUKUV8r/2EI4ngqJxD7iBdBYLhwluN30JaT3Q==";

const STEPS: &[&str] = &["cls", "librgw-rados", "harness", "cluster-tier", "model", "nsfs-librgw"];
const RADOS: &[&str] = &["cls", "librgw-rados", "harness", "cluster-tier"];

struct Opts {
    dry_run: bool,
    build: bool,
    build_dir: PathBuf,
    src: PathBuf,
    steps: Vec<&'static str>,
}

/// One command of the run:  what it is, how it is run, where its output goes.
struct Cmd {
    step: &'static str,
    what: String,
    argv: Vec<String>,
    cwd: PathBuf,
    sudo: bool,
}

fn main() {
    let o = parse_args();
    let run_dir = o.build_dir.join("fsio-validate").join(timestamp());
    let mut summary: Vec<String> = Vec::new();

    if !o.dry_run {
        if is_root() {
            die("run it as yourself, not root:  sudo is taken where it is needed");
        }
        if o.steps.iter().any(|s| RADOS.contains(s)) {
            if !std::io::stdin().is_terminal() {
                die("needs a terminal:  the sudo ticket is per tty, and the password is asked once, here");
            }
            take_sudo();
        }
        std::fs::create_dir_all(&run_dir).unwrap_or_else(|e| die(&format!("{}:  {e}", run_dir.display())));
        println!("run directory:  {}", run_dir.display());
    }

    if o.build {
        let mut targets = vec!["vstart-base", "erasure_code_plugins", "ceph-conf"];
        for s in &o.steps {
            targets.extend(match *s {
                "cls" => &["cls_rgw", "ceph_test_cls_rgw_fsio_head", "ceph_test_neorados_cls_rgw_fsio"][..],
                "librgw-rados" => &["rgw", "ceph_test_librgw_file_fsio_put"][..],
                "harness" => &["ceph_test_rgw_fsio_rados"][..],
                "cluster-tier" | "model" => &["ceph_test_cls_rgw_fsio_model"][..],
                "nsfs-librgw" => &["rgw", "ceph_test_librgw_file_write2", "ceph_test_librgw_file_nfsns"][..],
                _ => &[][..],
            });
        }
        let mut seen = std::collections::HashSet::new();
        targets.retain(|t| seen.insert(*t));
        let mut argv = vec!["ninja".to_string(), format!("-j{}", nproc())];
        argv.extend(targets.iter().map(|t| t.to_string()));
        let c = Cmd { step: "build", what: "build what the steps run".into(), argv, cwd: o.build_dir.clone(), sudo: false };
        if !exec(&o, &c, &run_dir, &mut summary, |_| None) {
            finish(&o, &run_dir, &summary, true);
        }
    }
    if !o.dry_run {
        check_version(&o);
    }

    let mut failed = false;
    let rados: Vec<_> = o.steps.iter().filter(|s| RADOS.contains(s)).copied().collect();
    if !rados.is_empty() {
        failed |= rados_phase(&o, &rados, &run_dir, &mut summary);
    }
    if o.steps.contains(&"nsfs-librgw") {
        failed |= nsfs_phase(&o, &run_dir, &mut summary);
    }
    if o.steps.contains(&"model") {
        let c = Cmd {
            step: "model",
            what: "the model's in-memory tiers".into(),
            argv: vec![o.src.join("src/test/cls_rgw/fsio_model_runs.rs").display().to_string(),
                       "--bin".into(), o.build_dir.join("bin/ceph_test_cls_rgw_fsio_model").display().to_string(),
                       "--out".into(), run_dir.join("model").display().to_string(),
                       "--jobs".into(), "8".into(), "quick".into(), "vectors".into(), "skips".into()],
            cwd: o.src.clone(),
            sudo: false,
        };
        failed |= !exec(&o, &c, &run_dir, &mut summary, expected_count);
    }
    finish(&o, &run_dir, &summary, failed);
}

/// A fresh RADOS vstart with fsio enabled, the RADOS steps, the cluster
/// stopped and the run's files handed back.  True if any step failed.
fn rados_phase(o: &Opts, steps: &[&'static str], run_dir: &Path, summary: &mut Vec<String>) -> bool {
    let b = &o.build_dir;
    let conf = b.join("ceph.conf").display().to_string();
    let sh = |step: &'static str, what: &str, line: String| Cmd {
        step, what: what.into(), argv: vec!["sh".into(), "-c".into(), line], cwd: b.clone(), sudo: true,
    };
    let setup = [
        sh("rados-stop", "stop whatever is running", "../src/stop.sh".into()),
        sh("rados-vstart", "a fresh RADOS vstart",
           "env MON=1 OSD=1 RGW=1 MGR=0 MDS=0 ../src/vstart.sh -n -d -o 'bluestore_block_size = 2T'".into()),
        sh("rados-fsio", "enable the fsio zonegroup feature",
           "bin/radosgw-admin -c ceph.conf zonegroup modify --rgw-zonegroup=default --enable-feature=fsio".into()),
    ];
    let mut failed = false;
    for c in &setup {
        let vstart = c.step == "rados-vstart";
        let ok = exec(o, c, run_dir, summary, |log| {
            (vstart && log.contains("expected plugin")).then(|| "a plugin version mismatch".to_string())
        });
        if !ok {
            failed = true;
            break;
        }
    }
    if !failed {
        for s in steps {
            let cmds: Vec<Cmd> = match *s {
                "cls" => vec![
                    sh("cls", "libcls_rgw calls no RGW symbol",
                       "! nm -D --undefined-only lib/libcls_rgw.so | c++filt | grep ' rgw_'".into()),
                    sh("cls", "the CLS head test", format!("env CEPH_CONF={conf} bin/ceph_test_cls_rgw_fsio_head")),
                    sh("cls", "the neorados CLS test", format!("env CEPH_CONF={conf} bin/ceph_test_neorados_cls_rgw_fsio")),
                ],
                "librgw-rados" => vec![sh("librgw-rados", "the librgw test", format!(
                    "env CEPH_CONF={conf} AWS_ACCESS_KEY_ID={AWS_KEY} AWS_SECRET_ACCESS_KEY='{AWS_SECRET}' \
                     bin/ceph_test_librgw_file_fsio_put --debug-rgw=10 --log-to-stderr=true"))],
                "harness" => vec![sh("harness", "the driver's harness", format!("env CEPH_CONF={conf} bin/ceph_test_rgw_fsio_rados"))],
                "cluster-tier" => {
                    let runner = match model_runner(o) {
                        Some(r) => r,
                        None => {
                            summary.push(line("cluster-tier", "the runner's build", false, "cargo reported no executable"));
                            failed = true;
                            continue;
                        }
                    };
                    vec![
                        sh("cluster-tier", "the origin check off, for the tier's many incarnations",
                           "bin/ceph -c ceph.conf config set osd rgw_fsio_check_origin false".into()),
                        Cmd { step: "cluster-tier", what: "the model's cluster tier".into(),
                              argv: vec!["env".into(), format!("CEPH_CONF={conf}"), runner,
                                         "--bin".into(), b.join("bin/ceph_test_cls_rgw_fsio_model").display().to_string(),
                                         "--out".into(), run_dir.join("cluster-tier").display().to_string(),
                                         "--jobs".into(), "8".into(), "cluster".into()],
                              cwd: o.src.clone(), sudo: true },
                    ]
                }
                _ => vec![],
            };
            for c in &cmds {
                failed |= !exec(o, c, run_dir, summary, expected_count);
            }
        }
    }
    let (user, group) = (id("-un"), id("-gn"));
    let teardown = [
        sh("rados-stop", "stop the cluster", "../src/stop.sh".into()),
        sh("rados-chown", "the run's files back to you", format!(
            "chown -R {user}:{group} asok dev out ceph.conf keyring rgwlib_readf.out {} 2>/dev/null; true",
            run_dir.display())),
    ];
    for c in &teardown {
        failed |= !exec(o, c, run_dir, summary, |_| None);
    }
    failed
}

/// An nsfs gateway from this tree's rgw-vstart.sh, the librgw suites, the
/// gateway stopped.  True if any step failed.
fn nsfs_phase(o: &Opts, run_dir: &Path, summary: &mut Vec<String>) -> bool {
    let b = &o.build_dir;
    let conf = format!("-c {}", b.join("ceph.conf").display());
    let sh = |what: &str, line: String| Cmd {
        step: "nsfs-librgw", what: what.into(), argv: vec!["sh".into(), "-c".into(), line], cwd: b.clone(), sudo: false,
    };
    let keys = format!("env AWS_ACCESS_KEY_ID={AWS_KEY} AWS_SECRET_ACCESS_KEY='{AWS_SECRET}'");
    let cmds = [
        sh("an nsfs gateway", "../src/script/rgw/rgw-vstart.sh --store nsfs --clean".into()),
        sh("write2", format!("{keys} bin/ceph_test_librgw_file_write2 --create --userid testid \"{conf}\"")),
        sh("nfsns, every group", format!(
            "{keys} bin/ceph_test_librgw_file_nfsns --create --userid testid \
             --hier1 --dirs1 --marker1 --setattr --rename --readf --writef \"{conf}\"")),
    ];
    let mut failed = false;
    for (i, c) in cmds.iter().enumerate() {
        let ok = exec(o, c, run_dir, summary, expected_count);
        failed |= !ok;
        if !ok && i == 0 {
            break;
        }
    }
    failed |= !exec(o, &sh("stop the gateway", "../src/stop.sh".into()), run_dir, summary, |_| None);
    failed
}

/// Runs one command, its output to `<run_dir>/<step>.log`, and records a
/// summary line.  `count` reads a result count from the output, or names
/// a failure the exit status does not show.  True if it succeeded.
fn exec(o: &Opts, c: &Cmd, run_dir: &Path, summary: &mut Vec<String>,
        count: impl Fn(&str) -> Option<String>) -> bool {
    let mut argv: Vec<String> = Vec::new();
    if c.sudo {
        argv.extend(["sudo".into(), "-n".into()]);
    }
    argv.extend(c.argv.iter().cloned());
    if o.dry_run {
        println!("[{}] {}\n  (cd {} && {})", c.step, c.what, c.cwd.display(), shell_words(&argv));
        return true;
    }
    println!("== [{}] {}", c.step, c.what);
    let log = run_dir.join(format!("{}.log", c.step));
    let mut f = std::fs::OpenOptions::new().create(true).append(true).open(&log)
        .unwrap_or_else(|e| die(&format!("{}:  {e}", log.display())));
    use std::io::Write;
    let _ = writeln!(f, "== {}\n$ (cd {} && {})", c.what, c.cwd.display(), shell_words(&argv));
    let out = Command::new(&argv[0]).args(&argv[1..]).current_dir(&c.cwd)
        .stdin(Stdio::null()).output();
    let (ok, text) = match out {
        Ok(out) => {
            let text = format!("{}{}", String::from_utf8_lossy(&out.stdout), String::from_utf8_lossy(&out.stderr));
            (out.status.success(), text)
        }
        Err(e) => (false, format!("could not run:  {e}\n")),
    };
    let _ = f.write_all(text.as_bytes());
    let note = count(&text).unwrap_or_default();
    let failed_note = !ok || note.starts_with("a plugin");
    let l = line(c.step, &c.what, !failed_note, &note);
    println!("{l}");
    summary.push(l);
    !failed_note
}

/// "[  PASSED  ] 18 tests." and "[  FAILED  ] 2 tests", or a tier's
/// "N of M as expected"
fn expected_count(text: &str) -> Option<String> {
    let mut parts: Vec<String> = Vec::new();
    for l in text.lines() {
        let t = l.trim();
        if t.starts_with("[  PASSED  ]") || (t.starts_with("[  FAILED  ]") && t.contains(" test")
            && t.chars().nth(13).map_or(false, |c| c.is_ascii_digit())) {
            parts.push(t.trim_start_matches("[  PASSED  ] ").trim_start_matches("[  FAILED  ] ")
                .trim_end_matches(", listed below:").trim_end_matches('.').to_string()
                + if t.starts_with("[  PASSED") { " passed" } else { " failed" });
        } else if t.contains(" as expected") && t.contains(" of ") {
            parts.push(t.to_string());
        }
    }
    (!parts.is_empty()).then(|| parts.join(", "))
}

fn line(step: &str, what: &str, ok: bool, note: &str) -> String {
    format!("{:<14} {:<4} {}{}", step, if ok { "ok" } else { "FAIL" }, what,
            if note.is_empty() { String::new() } else { format!(":  {note}") })
}

fn finish(o: &Opts, run_dir: &Path, summary: &[String], failed: bool) -> ! {
    if !o.dry_run {
        let text = summary.join("\n") + "\n";
        let _ = std::fs::write(run_dir.join("summary.txt"), &text);
        println!("\n== summary  ({})\n{text}", run_dir.join("summary.txt").display());
    }
    std::process::exit(if failed { 1 } else { 0 });
}

/// The cluster tier's runner, built as you;  its binary's path, which
/// cargo reports, so that root can run it
fn model_runner(o: &Opts) -> Option<String> {
    let manifest = o.src.join("src/test/cls_rgw/fsio_model_runs.rs");
    if o.dry_run {
        return Some("<fsio_model_runs, built by cargo>".into());
    }
    let out = Command::new("cargo").args(["+nightly", "-Zscript", "build", "--manifest-path"])
        .arg(&manifest).arg("--message-format=json").stderr(Stdio::null()).output().ok()?;
    let text = String::from_utf8_lossy(&out.stdout);
    text.lines().rev().find_map(|l| {
        let i = l.find("\"executable\":\"")? + "\"executable\":\"".len();
        let rest = &l[i..];
        Some(rest[..rest.find('"')?].to_string())
    })
}

/// The build must be HEAD's:  vstart runs what is in it
fn check_version(o: &Opts) {
    let head = capture(&o.src, &["git", "rev-parse", "--short=11", "HEAD"]);
    let lib = o.build_dir.join("lib/libec_jerasure.so");
    let built = std::fs::read(&lib).ok().and_then(|bytes| {
        let s = String::from_utf8_lossy(&bytes).into_owned();
        let i = s.find("22.0.0-")?;
        let tail: String = s[i..].chars().take_while(|c| c.is_ascii_alphanumeric() || *c == '.' || *c == '-').collect();
        tail.rsplit("-g").next().map(|h| h.to_string())
    });
    match built {
        Some(h) if h.starts_with(&head) || head.starts_with(&h) => {}
        Some(h) => die(&format!("the build is g{h}, HEAD is {head}:  build first (drop --no-build)")),
        None => die(&format!("no version in {}:  build first", lib.display())),
    }
}

fn take_sudo() {
    println!("sudo is needed for the RADOS vstart and its steps;  once:");
    if !Command::new("sudo").arg("-v").status().map(|s| s.success()).unwrap_or(false) {
        die("sudo -v failed");
    }
    std::thread::spawn(|| loop {
        std::thread::sleep(std::time::Duration::from_secs(60));
        let _ = Command::new("sudo").args(["-n", "-v"]).status();
    });
}

fn parse_args() -> Opts {
    let mut dry_run = false;
    let mut build = true;
    let mut build_dir: Option<PathBuf> = None;
    let mut steps: Vec<&'static str> = Vec::new();
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        match a.as_str() {
            "--dry-run" => dry_run = true,
            "--no-build" => build = false,
            "--build" => build_dir = Some(PathBuf::from(args.next().unwrap_or_else(|| die("--build needs a directory")))),
            "driver" => steps.extend(["cls", "librgw-rados", "harness"]),
            "core" => steps.extend(["cls", "cluster-tier", "model"]),
            "full" => steps.extend(STEPS),
            "-h" | "--help" => {
                println!("usage:  fsio_validate.rs [--dry-run] [--no-build] [--build DIR] STEP|PRESET...\n\
                          steps:  {}\npresets:  driver core full", STEPS.join(" "));
                std::process::exit(0);
            }
            s => match STEPS.iter().find(|x| **x == s) {
                Some(x) => steps.push(x),
                None => die(&format!("unknown step or preset {s}")),
            },
        }
    }
    if steps.is_empty() {
        die("name the steps or a preset:  --help lists them");
    }
    let mut ordered: Vec<&'static str> = STEPS.iter().copied().filter(|s| steps.contains(s)).collect();
    ordered.dedup();
    let src = PathBuf::from(capture(Path::new("."), &["git", "rev-parse", "--show-toplevel"]));
    let build_dir = build_dir.unwrap_or_else(|| src.join("build"));
    if !build_dir.join("build.ninja").exists() {
        die(&format!("{} is not a configured build directory", build_dir.display()));
    }
    Opts { dry_run, build, build_dir, src, steps: ordered }
}

fn capture(cwd: &Path, argv: &[&str]) -> String {
    let out = Command::new(argv[0]).args(&argv[1..]).current_dir(cwd).output()
        .unwrap_or_else(|e| die(&format!("{}:  {e}", argv.join(" "))));
    if !out.status.success() {
        die(&format!("{} failed", argv.join(" ")));
    }
    String::from_utf8_lossy(&out.stdout).trim().to_string()
}

fn id(flag: &str) -> String {
    capture(Path::new("."), &["id", flag])
}

fn is_root() -> bool {
    id("-u") == "0"
}

fn nproc() -> String {
    capture(Path::new("."), &["nproc"])
}

fn timestamp() -> String {
    capture(Path::new("."), &["date", "+%Y%m%d-%H%M%S"])
}

fn shell_words(argv: &[String]) -> String {
    argv.iter().map(|a| {
        if a.chars().all(|c| c.is_ascii_alphanumeric() || "-_./=:,+".contains(c)) {
            a.clone()
        } else {
            format!("'{}'", a.replace('\'', "'\\''"))
        }
    }).collect::<Vec<_>>().join(" ")
}

fn die(msg: &str) -> ! {
    eprintln!("fsio_validate:  {msg}");
    std::process::exit(2);
}
