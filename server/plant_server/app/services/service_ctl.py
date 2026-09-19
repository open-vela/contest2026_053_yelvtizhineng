# -*- coding: utf-8 -*-
"""服务自重启：管理台「重启服务」按钮的实现。

演示机跑的是裸 python 进程（python -u run_server.py --host 0.0.0.0 --port 8011），
所以做法是：另起一个**游离的看护进程** → 等旧进程把 HTTP 响应发完 → 杀掉旧进程 →
按同样的 argv / cwd / 环境变量（含 PLANT_MIMO_KEY）拉起新进程；
端口被 TIME_WAIT 占住起不来时自动重试最多 3 次。

云端（systemd / docker）交给外部监管更稳：设环境变量
`PLANT_RESTART_CMD`（例如 systemctl restart zhixiaoban），
管理台就只执行它，不再自己拉进程。
"""
import os
import re
import signal
import subprocess
import sys
import time
import urllib.request

BASE_DIR = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))


def _target(host=None, port=None):
    """优先复用本进程的启动参数；拿不到就按传入的 host/port 兜底。"""
    argv = list(sys.argv)
    if argv and argv[0].replace("\\", "/").endswith("run_server.py"):
        return argv, BASE_DIR
    return (["run_server.py", "--host", host or "0.0.0.0",
             "--port", str(port or 8011)], BASE_DIR)


def _log_paths():
    d = os.path.join(BASE_DIR, "data", "logs")
    os.makedirs(d, exist_ok=True)
    return (os.path.join(d, "server.log"), os.path.join(d, "server.err.log"),
            os.path.join(d, "restart.log"))


def _note(path, text):
    try:
        with open(path, "a", encoding="utf-8", errors="replace") as f:
            f.write("%s  %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), text))
    except OSError:
        pass


def _spawn(args, cwd, stdout=None, stderr=None):
    kw = {}
    if os.name == "nt":
        # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP：不弹窗、不随父进程退出
        kw["creationflags"] = 0x00000008 | 0x00000200
    else:
        kw["start_new_session"] = True
    return subprocess.Popen(args, cwd=cwd, close_fds=True,
                            stdin=subprocess.DEVNULL,
                            stdout=stdout if stdout else subprocess.DEVNULL,
                            stderr=stderr if stderr else subprocess.DEVNULL,
                            **kw)


def restart_later(host=None, port=None, delay=2.0, who="admin"):
    """安排一次重启，立刻返回（HTTP 响应能正常发出去）。"""
    out, err, rlog = _log_paths()
    _note(rlog, "管理台(%s) 请求重启服务，旧 pid=%s" % (who, os.getpid()))
    external = os.environ.get("PLANT_RESTART_CMD", "").strip()
    if external:
        _spawn([sys.executable, "-m", "app.services.service_ctl", "--run",
                str(delay), "--"] + external.split(), BASE_DIR)
        _note(rlog, "已交给外部命令重启：%s" % external)
        return True, "已交给外部命令重启：%s" % external
    cmd, cwd = _target(host, port)
    _spawn([sys.executable, "-m", "app.services.service_ctl", "--child",
            str(os.getpid()), str(delay), out, err, cwd, "--"] + cmd, cwd)
    return True, "已启动重启看护进程"


def start_detached(port=None, host=None, log_out=None, log_err=None):
    """游离启动（开机/重启后手动拉起来用）。返回新进程 pid。"""
    cmd, cwd = _target(host, port)
    default_out, default_err, _ = _log_paths()
    fo = open(log_out or default_out, "a", encoding="utf-8",
              errors="replace")
    fe = open(log_err or default_err, "a", encoding="utf-8",
              errors="replace")
    try:
        p = _spawn([sys.executable, "-u"] + cmd, cwd, stdout=fo, stderr=fe)
    finally:
        fo.close()
        fe.close()
    return p.pid


def _port_of(cmd):
    port = 8011
    for i, a in enumerate(cmd):
        if a == "--port" and i + 1 < len(cmd):
            try:
                port = int(cmd[i + 1])
            except ValueError:
                pass
    return port


def _alive(port, timeout=2.0):
    try:
        url = "http://127.0.0.1:%d/api/v1/health" % port
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return getattr(r, "status", 200) == 200
    except Exception:
        return False


def _kill(pid):
    try:
        if os.name == "nt":
            subprocess.run(["taskkill", "/PID", str(pid), "/F"],
                           capture_output=True)
        else:
            os.kill(pid, signal.SIGTERM)
            for _ in range(20):
                time.sleep(0.2)
                try:
                    os.kill(pid, 0)
                except OSError:
                    return
            os.kill(pid, signal.SIGKILL)
    except OSError:
        pass


def _exists(pid):
    """进程还在不在（用来确认旧进程真的死了，避免端口被旧进程占着）。"""
    if os.name == "nt":
        try:
            out = subprocess.run(["tasklist", "/FI", "PID eq %d" % pid],
                                 capture_output=True, text=True).stdout
        except OSError:
            return False
        return re.search(r"\b%d\b" % pid, out) is not None
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def _wait_gone(pid, timeout=12.0):
    end = time.time() + timeout
    while time.time() < end:
        if not _exists(pid):
            return True
        time.sleep(0.3)
    return not _exists(pid)


def _child(pid, delay, out, err, cwd, cmd):
    """看护进程：等一会儿 → 杀旧 → 起新 → 探活，失败重试。"""
    time.sleep(float(delay))
    _kill(pid)
    if not _wait_gone(pid):
        _note(out, "旧进程 pid=%s 没能停掉，放弃重启（避免端口被占）。" % pid)
        return 1
    time.sleep(0.8)
    port = _port_of(cmd)
    for attempt in range(1, 4):
        _note(out, "===== 服务重启，第 %d 次拉起（看护 pid=%s）=====" % (attempt, os.getpid()))
        fo = open(out, "a", encoding="utf-8", errors="replace")
        fe = open(err, "a", encoding="utf-8", errors="replace")
        try:
            p = _spawn([sys.executable, "-u"] + cmd, cwd, stdout=fo, stderr=fe)
        finally:
            fo.close()
            fe.close()
        for _ in range(30):
            time.sleep(0.5)
            if _alive(port):
                _note(out, "服务已恢复：新 pid=%s" % p.pid)
                return 0
            if p.poll() is not None:
                break
        _note(out, "第 %d 次没起来（退出码 %s），2 秒后重试" % (attempt, p.poll()))
        try:
            p.kill()
        except Exception:
            pass
        time.sleep(2.0)
    _note(out, "重启失败：三次都没起来，请手动启动 run_server.py")
    return 1


def _run(delay, cmd):
    time.sleep(float(delay))
    try:
        return subprocess.run(cmd).returncode
    except Exception:
        return 1


if __name__ == "__main__":
    args = sys.argv[1:]
    if args and args[0] == "--start":
        rest = args[1:]
        port = None
        if "--port" in rest:
            port = int(rest[rest.index("--port") + 1])
        pid = start_detached(port=port)
        print("started pid=%s port=%s" % (pid, port or 8011))
        sys.exit(0)
    if args and args[0] == "--child":
        rest = args[1:]
        cut = rest.index("--")
        head, cmd = rest[:cut], rest[cut + 1:]
        sys.exit(_child(int(head[0]), head[1], head[2], head[3], head[4], cmd))
    if args and args[0] == "--run":
        rest = args[1:]
        cut = rest.index("--")
        sys.exit(_run(rest[0], rest[cut + 1:]))
    print(__doc__)
