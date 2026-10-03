"""Scenario engine: build steps -> companions -> VM boot(s) with host-side actions -> artifacts -> assertions.

Runs inside WSL (Arch) next to qemu. Stdlib only. See pk/__init__.py for the design rules and
asserts.Ctx for the trust model (scenarios are repo-authored recipes, like a Makefile).
"""
import json
import os
import shutil
import re
import signal
import socket
import subprocess
import threading
import time

from . import asserts as A

PASS, FAIL, WARN, SKIP = "PASS", "FAIL", "WARN", "SKIP"


# ------------------------------------------------------------------------------------------- helpers
def expand(obj, vars_):
    if isinstance(obj, str):
        for k, v in vars_.items():
            obj = obj.replace("${%s}" % k, v)
        return obj
    if isinstance(obj, list):
        return [expand(x, vars_) for x in obj]
    if isinstance(obj, dict):
        return {k: expand(v, vars_) for k, v in obj.items()}
    return obj


def sh(cmd, root, log, env=None, timeout=3000):
    e = dict(os.environ)
    e.update({k: str(v) for k, v in (env or {}).items()})
    with open(log, "wb") as fh:
        p = subprocess.Popen(["bash", "-c", cmd], cwd=root, stdout=fh, stderr=subprocess.STDOUT, env=e)
        try:
            return p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            p.kill()
            return 124


def tail_has(path, rx):
    try:
        return re.search(rx, open(path, "rb").read().decode("latin1")) is not None
    except OSError:
        return False


def qmp_screendump(sock_path, out_png):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(10)
        s.connect(sock_path)
        f = s.makefile("rw")
        f.readline()

        def cmd(o):
            f.write(json.dumps(o) + "\n")
            f.flush()
            while True:
                line = f.readline()
                if not line or '"return"' in line or '"error"' in line:
                    return line
        cmd({"execute": "qmp_capabilities"})
        r = cmd({"execute": "screendump", "arguments": {"filename": out_png, "format": "png"}})
        return os.path.exists(out_png) and '"error"' not in r
    except Exception:  # noqa: BLE001
        return False


# ------------------------------------------------------------------------------------------- the run
class Run:
    def __init__(self, scenario, root, opts):
        self.sc, self.root, self.opts = scenario, root, opts
        self.name = scenario["name"]
        self.art = os.path.join(root, "build", "proof", self.name)
        os.makedirs(self.art, exist_ok=True)
        self.vars = {"ROOT": root, "ART": self.art, "NAME": self.name}
        self.ctx = A.Ctx(self.art, root)
        self.results = []          # [{id, stage, status, evidence, why}]
        self.build_ok = True

    # ---- build ----------------------------------------------------------------------------
    def build(self):
        for st in self.sc.get("build", []):
            st = expand(st, self.vars)
            rid = "build." + st["name"]
            envskip = st.get("skip_if_env")
            if self.opts.get("no_build") or (envskip and os.environ.get(envskip)):
                self.results.append(dict(id=rid, stage="build", status=SKIP, evidence="build skipped", why=st.get("why", "")))
                continue
            log = os.path.join(self.art, "build_%s.log" % st["name"])
            t0 = time.time()
            rc = sh(st["cmd"], self.root, log, st.get("env"), st.get("timeout", 3000))
            text = open(log, "rb").read().decode("latin1")
            bad = [p for p in st.get("fail_if", ["error:", "undefined reference", r"^FAIL"])
                   if re.search(p, text, re.M)]
            ok = rc == 0 and not bad
            ev = "%.0fs rc=%d" % (time.time() - t0, rc)
            if bad:
                m = next((l for l in text.splitlines() if re.search(bad[0], l)), "")
                ev += "  matched %r: %s" % (bad[0], m.strip()[:140])
            self.results.append(dict(id=rid, stage="build", status=PASS if ok else FAIL, evidence=ev, why=st.get("why", "")))
            if not ok:
                self.build_ok = False
                break

    # ---- companions (host services running for the whole scenario) ------------------------------
    def start_companions(self):
        procs = []
        for c in self.sc.get("companions", []):
            c = expand(c, self.vars)
            log = os.path.join(self.art, "companion_%s.log" % c["name"])
            env = dict(os.environ)
            env.update({k: str(v) for k, v in c.get("env", {}).items()})
            fh = open(log, "wb")
            p = subprocess.Popen(["bash", "-c", c["cmd"]], cwd=self.root, stdout=fh, stderr=subprocess.STDOUT,
                                 env=env, preexec_fn=os.setsid)
            procs.append((p, fh))
            self.ctx.companions[c["name"]] = log
            if c.get("ready"):
                t0 = time.time()
                while time.time() - t0 < c.get("ready_timeout", 20) and not tail_has(log, c["ready"]):
                    time.sleep(0.3)
        return procs

    @staticmethod
    def stop(procs):
        for p, fh in procs:
            try:
                os.killpg(os.getpgid(p.pid), signal.SIGTERM)
            except (ProcessLookupError, PermissionError):
                pass
            fh.close()

    # ---- one VM boot ---------------------------------------------------------------------------
    def boot(self, vm):
        vm = expand(vm, self.vars)
        label = vm.get("label", "vm")
        serial = os.path.join(self.art, label + ".serial.log")
        pcap = os.path.join(self.art, label + ".pcap")
        qmp = "/tmp/pk_%s_%s.qmp" % (self.name, label)
        # The LIVE serial log and packet capture go to the WSL-native disk and are copied into the artifact folder when the VM stops.
        # Writing them straight onto the Windows-mounted artifact dir (/mnt/c, 9p/drvfs) is slow, and a guest serial write BLOCKS the
        # vCPU: a chatty guest (the 60 s syscall storms print ~10k trace lines/s) was throttled ~60x by the host's file I/O, which looked
        # exactly like a kernel hang. Found by the multi-core proof (bkl_storm) -- same kernel, same ISO, only the log location differed.
        fast_serial = "/tmp/pk_%s_%s.serial.log" % (self.name, label)
        fast_pcap = "/tmp/pk_%s_%s.pcap" % (self.name, label)
        for f in (serial, pcap, qmp, fast_serial, fast_pcap):
            try:
                os.remove(f)
            except OSError:
                pass
        iso = vm["iso"] if os.path.isabs(vm["iso"]) else os.path.join(self.root, vm["iso"])
        nd = vm.get("netdev", "user,id=n0")
        if vm.get("hostfwd"):
            nd += "".join(",hostfwd=" + h for h in vm["hostfwd"])
        argv = ["qemu-system-x86_64", "-cdrom", iso, "-m", str(vm.get("mem", 512)), "-smp", str(vm.get("smp", 1)),
                "-netdev", nd, "-device", "%s,netdev=n0" % vm.get("nic", "e1000"),
                "-display", "none", "-serial", "file:" + fast_serial, "-qmp", "unix:%s,server,nowait" % qmp, "-no-reboot"]
        if vm.get("cpu"):
            argv += ["-cpu", vm["cpu"]]
        if vm.get("pcap", True):
            argv += ["-object", "filter-dump,id=f1,netdev=n0,file=" + fast_pcap]
        argv += vm.get("extra", [])
        rec = {"serial": fast_serial, "pcap": fast_pcap if vm.get("pcap", True) else None, "shots": {}}   # repointed at the artifacts below
        self.ctx.vms[label] = rec
        if self.ctx.default_vm is None:
            self.ctx.default_vm = label
        qp = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        t0, timeout = time.time(), vm.get("timeout", 90)
        ready, until = vm.get("ready"), vm.get("until")
        actions = [expand(a, self.vars) for a in self.sc.get("actions", []) if a.get("vm", label) == label]
        threads, started = [], set()

        def run_action(a):
            log = os.path.join(self.art, "action_%s.log" % a["name"])
            env = dict(os.environ)
            env.update({k: str(v) for k, v in a.get("env", {}).items()})
            env["PK_SERIAL"] = fast_serial
            with open(log, "wb") as fh:
                try:
                    r = subprocess.run(["bash", "-c", a["cmd"]], cwd=self.root, stdout=fh, stderr=subprocess.STDOUT,
                                       env=env, timeout=a.get("timeout", 300))
                    rc = r.returncode
                except subprocess.TimeoutExpired:
                    rc = 124
            self.ctx.actions[a["name"]] = {"exit": rc, "log": log}

        while time.time() - t0 < timeout:
            if qp.poll() is not None:
                break
            for a in actions:
                if a["name"] not in started and (not a.get("when") or tail_has(fast_serial, a["when"])):
                    started.add(a["name"])
                    th = threading.Thread(target=run_action, args=(a,), daemon=True)
                    th.start()
                    threads.append(th)
            done_actions = all(a["name"] in started for a in actions) and not any(t.is_alive() for t in threads)
            if until and tail_has(fast_serial, until) and done_actions:
                break
            time.sleep(1.0)
        for th in threads:                       # an action may outlive the `until` match
            th.join(timeout=vm.get("action_grace", 120))
        time.sleep(vm.get("settle", 5))
        for shot in vm.get("screenshots", ["final"]):
            png = os.path.join(self.art, "%s_%s.png" % (label, shot))
            if qmp_screendump(qmp, png):
                rec["shots"][shot] = png
        qp.terminate()
        try:
            qp.wait(timeout=5)
        except subprocess.TimeoutExpired:
            qp.kill()
        rec["boot_seconds"] = round(time.time() - t0, 1)
        for src, dst in ((fast_serial, serial), (fast_pcap, pcap)):      # publish the artifacts; assertions read these copies
            try:
                shutil.copyfile(src, dst)
            except OSError:
                pass
        rec["serial"] = serial
        rec["pcap"] = pcap if vm.get("pcap", True) else None

    # ---- evaluation -----------------------------------------------------------------------------
    def evaluate(self):
        status = {r["id"]: r["status"] for r in self.results}
        for spec in self.sc.get("assertions", []):
            aid, typ = spec["id"], spec["type"]
            deps = spec.get("depends_on", [])
            bad_dep = next((d for d in deps if status.get(d) not in (PASS, WARN)), None)
            if bad_dep:
                res = dict(id=aid, stage=spec.get("stage", ""), status=SKIP, why=spec.get("why", ""),
                           evidence="skipped: depends on %s which is %s" % (bad_dep, status.get(bad_dep, "missing")))
            elif not self.build_ok and spec.get("needs_build", True) and typ not in ("cmd", "file"):
                res = dict(id=aid, stage=spec.get("stage", ""), status=SKIP, why=spec.get("why", ""),
                           evidence="skipped: build failed")
            else:
                try:
                    ok, ev = A.TYPES[typ](self.ctx, spec)
                except KeyError:
                    ok, ev = False, "unknown assertion type %r" % typ
                except Exception as e:  # noqa: BLE001
                    ok, ev = False, "assertion crashed: %s" % e
                st = PASS if ok else (WARN if spec.get("severity") == "warn" else FAIL)
                res = dict(id=aid, stage=spec.get("stage", ""), status=st, evidence=ev, why=spec.get("why", ""))
            self.results.append(res)
            status[aid] = res["status"]


def run_scenario(path, root, opts):
    sc = json.load(open(path))
    r = Run(sc, root, opts)
    if opts.get("eval_only"):
        # reconstruct the artifact map from an earlier run
        for vm in sc.get("vms", [sc["vm"]] if "vm" in sc else []):
            label = vm.get("label", "vm")
            rec = {"serial": os.path.join(r.art, label + ".serial.log"), "pcap": os.path.join(r.art, label + ".pcap"), "shots": {}}
            for shot in vm.get("screenshots", ["final"]):
                p = os.path.join(r.art, "%s_%s.png" % (label, shot))
                if os.path.exists(p):
                    rec["shots"][shot] = p
            r.ctx.vms[label] = rec
            r.ctx.default_vm = r.ctx.default_vm or label
        for a in sc.get("actions", []):
            log = os.path.join(r.art, "action_%s.log" % a["name"])
            if os.path.exists(log):
                r.ctx.actions[a["name"]] = {"exit": 0, "log": log}
        r.evaluate()
        return r
    r.build()
    if r.build_ok:
        procs = r.start_companions()
        try:
            for vm in sc.get("vms", [sc["vm"]] if "vm" in sc else []):
                if opts.get("no_vm"):
                    break
                r.boot(vm)
        finally:
            r.stop(procs)
    r.evaluate()
    return r
