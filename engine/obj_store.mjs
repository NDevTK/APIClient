/* WHERE A NAMED ARTIFACT IS PUBLISHED, AND WHAT THE NAME IS A FACT ABOUT — one answer, for every tool here.
 *
 * WHY IT IS ITS OWN FILE, AND IT IS THE SAME ARGUMENT `engine/lexbor_source.mjs` MAKES FOR ITSELF. That file
 * says one archive-identity answer must be imported by everything that caches an archive, because "a second
 * copy of it is the same shape as the defect it removes: two programs deciding independently whether the same
 * source changed". The two primitives below were `engine/build.mjs`'s alone — the `APICLIENT_OBJ_STORE`
 * resolution and the version-text identity every object in that store is named under — and the second consumer
 * of a cached archive (`engine/wpt.mjs`, through `lexborNativeArchive`) could reach NEITHER. So the native
 * lexbor archive stayed a FIXED NAME plus a SIDECAR in a per-snapshot directory and every frozen build paid its
 * cmake+make, for want of a place to put two helpers rather than for any reason about cmake.
 *
 * NOTHING HERE DERIVES THE LAYOUT, which is `lexbor_source.mjs`'s rule for `srcDir` applied to the store: the
 * engine directory is taken as an ARGUMENT, because the callers reach it by their own constants and a copy
 * guessed at here would be another place the layout is written down.
 *
 * AND NOTHING HERE RUNS AT IMPORT. Every export is a function. `build.mjs`'s §OBJECT CACHE banner argues at
 * length that nothing may spawn a compiler above the branch that decides what to compile — so a module it
 * imports at the top of the file may not spawn one either, and a module-scope `mkdirSync` would create a store
 * for `--list-sources` to answer a question into.
 */
import { closeSync, existsSync, linkSync, lstatSync, mkdirSync, openSync, readdirSync, readFileSync, readSync,
         renameSync, rmSync, statSync, utimesSync, writeFileSync } from "node:fs";
import { basename, dirname, join, resolve } from "node:path";
import { createHash } from "node:crypto";
import { spawnSync } from "node:child_process";

/* THE STORE. `APICLIENT_OBJ_STORE` is set by `engine/frozen_snapshot.sh` to one directory outside every
   snapshot; unset, the fallback is this tree's own `.work/obj` and nothing is shared. The caller is told WHICH
   it got rather than left to re-read the environment, because a second read of one variable is a second answer
   to one question — and the two answers would be the banner and the behaviour.
   Every reader and writer of the store goes through the returned methods, which keep its protocol:
   - a name is published only by `rename` of this process's own `.tmp-<owner>-*` file inside the store, so a
     partial write is never visible under a final name;
   - a process reads an object only through its hard link in `.pins-<owner>/`, made when the hit is resolved
     (or before the name is published), so removing a store name never takes bytes from a running build;
   - every hit and every publish sets the entry's mtime to now, so mtime is the last use;
   - only `prune` removes store names, least recently used first, and never a name a live process pinned. */
export function objStore(engineDir) {
  const env = process.env.APICLIENT_OBJ_STORE || "";
  const dir = env ? resolve(env) : join(engineDir, ".work", "obj");
  mkdirSync(dir, { recursive: true });
  return {
    dir, shared: !!env,
    tmp: (suffix) => join(dir, ".tmp-" + ownerToken() + "-" + suffix),
    pin: (name) => pinObject(dir, name),
    publish: (tmp, name) => publishObject(dir, tmp, name),
    readRecord: (name) => readRecord(dir, name),
    writeRecord: (name, text) => writeRecord(dir, name, text),
    prune: (maxBytes) => prune(dir, maxBytes),
    pruneAtExit: (maxBytes) => { CHECK(Number.isSafeInteger(maxBytes) && maxBytes >= 0, "a cap is whole bytes");
                                  exitPrunes.set(dir, maxBytes); },
  };
}

/* The store's integrity checks are always fatal: an object name that answered with bytes no compiler wrote
   would be linked as this revision's program. */
function CHECK(cond, msg) {
  if (!cond) throw new Error("[objstore] CHECK failed: " + msg);
}

/* The names the store holds. An object is linked; a record is a dependency hint read at resolution only. */
const OBJECT_NAME = /^(?:[0-9a-f]{64}\.o|liblexbor-[0-9a-f]{32}\.o|liblexbor_static-[0-9a-f]{32}\.a)$/;
const ARCHIVE_NAME = /^liblexbor/;
const RECORD_NAME = /^[0-9a-f]{32}\.deps$/;
/* The object formats the compilers here emit: wasm (emcc), ELF (clang, gcc), and an ar archive. */
const OBJECT_MAGICS = [Buffer.from("\0asm", "latin1"), Buffer.from("\x7fELF", "latin1"),
                       Buffer.from("!<arch>\n", "latin1")];

function checkObjectBytes(path, name) {
  const size = statSync(path).size;
  const head = Buffer.alloc(8);
  const fd = openSync(path, "r");
  const got = readSync(fd, head, 0, head.length, 0);
  closeSync(fd);
  CHECK(size > 0 && OBJECT_MAGICS.some((m) => got >= m.length && head.subarray(0, m.length).equals(m)),
        `${path} (store name ${name}) is ${size} bytes beginning ${head.subarray(0, got).toString("hex")}, ` +
        "which is no object format a compiler here emits; a store name maps to a whole object or to nothing");
}

const touch = (path) => { const now = new Date(); utimesSync(path, now, now); };

/* WHO MADE AN ENTRY, AS A TOKEN THE PROCESS TABLE CAN CONFIRM: `<pid>@<boot>.<start>`, the boot id's first
   eight hex digits and the process start time in clock ticks since that boot, both read from /proc. A pid
   alone names whichever process holds it now — measured: a dead build's `.pin-31290` was answered "alive" by
   `kill -0` while pid 31290 was a PerfettoTrace process. The token needs no wall clock, so a clock step
   cannot make a live build look dead.
   NAMED RESIDUAL: a host without /proc gets `<pid>@pid` and liveness by `kill(pid, 0)`. What the next diff
   builds is the platform's own process creation time; its absence shows as pin directories of dead builds
   surviving a prune on such a host, which retains bytes and never takes them from a live build. */
let OWNER = null;
let BOOT = null;
function bootId() {
  if (BOOT === null) {
    BOOT = readFileSync("/proc/sys/kernel/random/boot_id", "utf8").trim().replace(/-/g, "").slice(0, 8);
    CHECK(/^[0-9a-f]{8}$/.test(BOOT), "boot_id reads " + JSON.stringify(BOOT));
  }
  return BOOT;
}
/* `/proc/<pid>/stat` fields after the parenthesised command name, from field 3 (state); field 22 is the start
   time. Null when there is no such process. */
function procStat(pid) {
  let text;
  try { text = readFileSync("/proc/" + pid + "/stat", "utf8"); }
  catch (e) { if (e.code === "ENOENT" || e.code === "ESRCH") return null; throw e; }
  const f = text.slice(text.lastIndexOf(")") + 2).split(" ");
  CHECK(/^[0-9]+$/.test(f[19]), "/proc/" + pid + "/stat field 22 reads " + JSON.stringify(f[19]));
  return { state: f[0], start: f[19] };
}
function ownerToken() {
  if (OWNER === null) {
    if (!existsSync("/proc/self/stat")) OWNER = process.pid + "@pid";
    else OWNER = process.pid + "@" + bootId() + "." + procStat("self").start;
  }
  return OWNER;
}
const pidExists = (pid) => {
  try { process.kill(pid, 0); return true; }
  catch (e) { if (e.code === "ESRCH") return false; if (e.code === "EPERM") return true; throw e; }
};
function ownerLive(token) {
  const m = /^([0-9]+)@(?:([0-9a-f]{8})\.([0-9]+)|pid)$/.exec(token);
  CHECK(m, "an owner token is <pid>@<boot>.<start> or <pid>@pid, not " + JSON.stringify(token));
  if (!m[2]) return pidExists(Number(m[1]));
  if (m[2] !== bootId()) return false;
  const st = procStat(Number(m[1]));
  return !!st && st.state !== "Z" && st.state !== "X" && st.start === m[3];
}
/* A build at a revision before this protocol names its entries by pid alone (`.pin-<pid>`, `.tmp-<pid>-*`),
   pins only `.o` hits, and pins an object only after publishing it. Its pid is live only while the process
   holding it runs a `build.mjs`, which a reused pid does not. */
function legacyOwnerLive(pid) {
  if (!existsSync("/proc/self/stat")) return pidExists(pid);
  let argv;
  try { argv = readFileSync("/proc/" + pid + "/cmdline", "utf8").split("\0"); }
  catch (e) { if (e.code === "ENOENT" || e.code === "ESRCH") return false; throw e; }
  return argv.some((a) => basename(a) === "build.mjs");
}

/* This process's pin directory per store, made on first use and removed at exit. A directory already standing
   under this process's own token is impossible, since the token names this process. */
const pinDirs = new Map();
const exitPrunes = new Map();
function pinDirOf(dir) {
  let p = pinDirs.get(dir);
  if (!p) {
    p = join(dir, ".pins-" + ownerToken());
    mkdirSync(p);
    if (pinDirs.size === 0) process.on("exit", atExit);
    pinDirs.set(dir, p);
  }
  return p;
}
/* Runs only in a process that pinned something, so a build that used no store object prunes nothing. The pins
   go first, so the prune that follows sees this build's objects as unpinned and most recently used. */
function atExit() {
  for (const p of pinDirs.values()) rmSync(p, { recursive: true, force: true });
  for (const [dir, maxBytes] of exitPrunes) prune(dir, maxBytes);
}

/* A hit: the pinned path the caller reads, or null when the store holds no such name (a miss). */
function pinObject(dir, name) {
  CHECK(OBJECT_NAME.test(name), "a store object name, not " + JSON.stringify(name));
  const pin = join(pinDirOf(dir), name);
  if (!existsSync(pin)) {
    try { linkSync(join(dir, name), pin); } catch (e) { if (e.code === "ENOENT") return null; throw e; }
  }
  checkObjectBytes(pin, name);
  touch(pin);
  return pin;
}

/* Publishes this process's complete temporary under `name` and returns its pin. The pin is linked before the
   rename, so no prune can remove the object between its publish and its use. */
function publishObject(dir, tmp, name) {
  CHECK(OBJECT_NAME.test(name), "a store object name, not " + JSON.stringify(name));
  CHECK(dirname(tmp) === dir && basename(tmp).startsWith(".tmp-" + ownerToken() + "-"),
        tmp + " is not this process's temporary in " + dir + ", and only a rename within it is atomic");
  checkObjectBytes(tmp, name);
  const pin = join(pinDirOf(dir), name);
  CHECK(!existsSync(pin), "this process publishes " + name + " after already pinning it");
  linkSync(tmp, pin);
  renameSync(tmp, join(dir, name));
  touch(pin);
  return pin;
}

/* A record is read once, at resolution, so it is not pinned; null is "no record", whether it was never written
   or was evicted. */
function readRecord(dir, name) {
  CHECK(RECORD_NAME.test(name), "a store record name, not " + JSON.stringify(name));
  const f = join(dir, name);
  try { touch(f); return readFileSync(f, "utf8"); }
  catch (e) { if (e.code === "ENOENT") return null; throw e; }
}
function writeRecord(dir, name, text) {
  CHECK(RECORD_NAME.test(name), "a store record name, not " + JSON.stringify(name));
  const tmp = join(dir, ".tmp-" + ownerToken() + "-" + name);
  writeFileSync(tmp, text);
  renameSync(tmp, join(dir, name));
}

/* EVICTS LEAST-RECENTLY-USED NAMES UNTIL THE STORE'S DISK USE IS AT MOST `maxBytes`. It first removes the pin
   directories and temporaries of processes that no longer run, then counts every inode under the store once,
   then unlinks unpinned names oldest mtime first. A name pinned by a live process is never evicted; while a
   legacy build runs, archives and anything written in the last minute are kept as well, because that build
   reads its archives unpinned and pins a fresh object just after publishing it. An unlink frees disk only when
   it drops the last link. A name gone before its unlink was evicted by a concurrent prune, which is the one
   other remover. Returns the census it printed. */
const OWNED_PIN = /^\.pins-(.+)$/;
const LEGACY_PIN = /^\.pin-([0-9]+)$/;
const OWNED_TMP = /^\.tmp-([0-9]+@(?:[0-9a-f]{8}\.[0-9]+|pid))-/;
const LEGACY_TMP = /^\.tmp-([0-9]+)-/;
function prune(dir, maxBytes) {
  CHECK(Number.isSafeInteger(maxBytes) && maxBytes >= 0, "a cap is whole bytes, not " + maxBytes);
  const now = Date.now();
  const inodes = new Map();
  const count = (st) => { inodes.set(st.dev + ":" + st.ino, st.blocks * 512); };
  const pinned = new Set();
  let legacyLive = false, deadPins = 0, deadTmps = 0;
  const named = [];
  const listPins = (p) => {
    let pins;
    try { pins = readdirSync(p); } catch (e) { if (e.code === "ENOENT") return; throw e; }
    for (const n of pins) {
      pinned.add(n);
      const st = lstatSync(join(p, n), { throwIfNoEntry: false });
      if (st) count(st);
    }
  };
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const n = e.name, path = join(dir, n);
    let m;
    if ((m = OWNED_PIN.exec(n)) || (m = LEGACY_PIN.exec(n))) {
      CHECK(e.isDirectory(), path + " is a pin name and not a directory");
      const legacy = m[0].startsWith(".pin-");
      if (legacy ? legacyOwnerLive(Number(m[1])) : ownerLive(m[1])) {
        legacyLive ||= legacy;
        listPins(path);
      } else { rmSync(path, { recursive: true, force: true }); deadPins++; }
    } else if ((m = OWNED_TMP.exec(n)) || (m = LEGACY_TMP.exec(n))) {
      const live = m[1].includes("@") ? ownerLive(m[1]) : legacyOwnerLive(Number(m[1]));
      if (!live) { rmSync(path, { force: true }); deadTmps++; continue; }
      const st = lstatSync(path, { throwIfNoEntry: false });
      if (st) count(st);
    } else {
      CHECK(e.isFile() && (OBJECT_NAME.test(n) || RECORD_NAME.test(n)),
            path + " is no entry the store protocol writes");
      const st = lstatSync(path, { throwIfNoEntry: false });
      if (st) { count(st); named.push({ n, st }); }
    }
  }
  let total = 0;
  for (const b of inodes.values()) total += b;
  const before = total;
  let kept = 0, keptLegacy = 0, evicted = 0, lastEvicted = null;
  const candidates = named.filter(({ n, st }) => {
    if (pinned.has(n)) { kept++; return false; }
    if (legacyLive && (ARCHIVE_NAME.test(n) || st.mtimeMs > now - 60_000)) { keptLegacy++; return false; }
    return true;
  }).sort((a, b) => a.st.mtimeMs - b.st.mtimeMs);
  for (const { n, st } of candidates) {
    if (total <= maxBytes) break;
    try { rmSync(join(dir, n)); } catch (e) { if (e.code === "ENOENT") continue; throw e; }
    evicted++;
    lastEvicted = st.mtimeMs;
    if (st.nlink === 1) total -= st.blocks * 512;
  }
  const mb = (b) => (b / 1048576).toFixed(0) + " MB";
  const census = { dir, maxBytes, before, after: total, named: named.length, evicted, lastEvicted,
                   pinned: kept, keptLegacy, deadPins, deadTmps };
  console.log(`[objstore] prune ${dir}: ${mb(before)} -> ${mb(total)} on disk (cap ${mb(maxBytes)}); ` +
              `evicted ${evicted} of ${named.length} names` +
              (lastEvicted === null ? "" : `, newest evicted last used ${new Date(lastEvicted).toISOString()}`) +
              `; ${kept} pinned by live builds, ${keptLegacy} kept for a legacy build; ` +
              `removed ${deadPins} dead pin dir(s) and ${deadTmps} dead temporaries`);
  return census;
}

/* WHAT A TOOL IS, AS A STRING THAT MOVES WHEN THE TOOL DOES — the whole of why a content name works.
 *
 * THE TOOLCHAIN IS ASKED FOR ITS VERSION, NOT POINTED AT, and the paragraph this came from in `build.mjs`
 * records what pointing at it cost: hashing the PATH `EMCC` under a comment claiming "the compiler binary is in
 * the hash for the same reason a header is" — and emsdk upgrades IN PLACE, so one path named two compilers that
 * emit different objects. That is a FALSE HIT rather than a miss, which is the one failure a content-addressed
 * store may not have.
 *
 * THE ACCEPTANCE TEST TRAVELS WITH THE TOOL because "did this report a version" is a different question per
 * tool: a compiler that cannot be identified may not name an artifact at all, since a name that does not change
 * when the compiler does is how a stale object is reported fresh. MEASURED, on this box, that the test cannot
 * be shared: `cc --version` is gcc here and its first line is `cc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0`,
 * which contains no `version` token at all, while `clang --version` opens `Ubuntu clang version 18.1.3` — so
 * one tight regex refuses the other tool, and the caller states its own.
 *
 * `pathIndependent` IS THE CALLER'S, NOT THIS FILE'S, and it is required rather than defaulted. WHERE this tree
 * sits is not part of any identity — `build.mjs` measured that four absolute `-I` roots alone made two
 * checkouts of ONE revision share nothing — but WHICH absolute prefixes may be collapsed is a claim about what
 * two checkouts share at that prefix, and only the caller knows: emsdk is one symlinked directory in every
 * frozen snapshot, so collapsing it is sound, and a path nobody shares collapsed anyway is a FALSE HIT. A
 * caller with nothing to collapse passes an identity function and says why at its own site.
 *
 * A TOOL THAT IS NOT INSTALLED AND A TOOL THAT FAILED ARE DIFFERENT FACTS, and only one of them writes to
 * stderr: a spawn that never started answers `status: null` with an empty `stderr` and puts the reason in
 * `error`. The refusal below reports which, because `lexbor_source.mjs` had already paid for printing the
 * stream alone — a bare failure line and a blank line, on a box with no cmake on it. */
export function toolIdentity(name, cc, versionArgv, versionOk, flags, pathIndependent) {
  const v = spawnSync(cc, versionArgv, { encoding: "utf8" });
  /* `InstalledDir` NAMES WHERE THE TOOL IS, WHICH IS NOT WHAT IT IS — clang prints an absolute one and it moves
     with the box rather than with the compiler. */
  const text = pathIndependent(((v.stdout || "") + (v.stderr || "")).split("\n")
                                 .filter((l) => !l.startsWith("InstalledDir")).join("\n"));
  if (!versionOk.test(text)) {
    console.error("[identity] `" + name + " " + versionArgv.join(" ") + "` did not report a version — " +
                  (v.error ? "it could not be run at all (" + (v.error.code || v.error.message) + ")"
                           : "it exited " + (v.status === null ? "on signal " + v.signal : v.status)) + "\n" +
                  "[identity]   an artifact may not be named for a tool this cannot identify — a name that\n" +
                  "[identity]   does not change when the tool does is how a stale object is reported fresh.");
    process.exit(1);
  }
  return createHash("sha256").update(text + "\0" + pathIndependent(flags.join("\0")))
                             .digest("hex").slice(0, 12);
}

/* A CMAKE CACHE IS CMAKE'S OWN ANSWER ABOUT WHAT IT PICKED, which is why it is read rather than predicted.
   `CMAKE_C_COMPILER` is resolved by cmake from `CMAKE_C_COMPILER`, then `$CC`, then a platform candidate list,
   and re-deriving that order here would be a second copy of a rule this file cannot be right about — and being
   wrong about it is a FALSE HIT, since an archive gcc emitted would keep the name clang's asks for. MEASURED
   from a real frozen build's cache: `CMAKE_C_COMPILER:FILEPATH=/usr/bin/cc`, which resolves to
   `x86_64-linux-gnu-gcc-13` on this box and is NOT the `clang` the native target compiles its own sources with.
   `CMAKE_HOME_DIRECTORY` IS WHY A CACHE MAY BE REUSED OR MAY NOT: it records WHERE the source was, and cmake
   REFUSES to reconfigure across a move ("The source … does not match the source … used to generate cache"), so
   a cache naming another source directory answers about another configuration.
   THE KEYS ARE `:TYPE=VALUE`, and the type is dropped: the caller asks for a variable and a cache that records
   it as `FILEPATH` today and something else tomorrow is the same answer. Absent is returned as null, which is a
   positive answer — "this cache does not record that" — and never an empty string a caller could hash. */
export function cmakeCacheVars(cacheFile, names) {
  let text;
  try { text = readFileSync(cacheFile, "utf8"); } catch { return null; }
  const out = Object.fromEntries(names.map((n) => [n, null]));
  for (const line of text.split("\n")) {
    const m = /^([A-Za-z0-9_]+):[A-Za-z]+=(.*)$/.exec(line.trim());
    if (m && Object.prototype.hasOwnProperty.call(out, m[1]) && out[m[1]] === null) out[m[1]] = m[2];
  }
  return out;
}
