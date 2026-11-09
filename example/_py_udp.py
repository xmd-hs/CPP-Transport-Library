#!/usr/bin/env python3
import socket
import subprocess
import threading

ip = subprocess.check_output(
    ["bash", "-lc", "hostname -I | awk '{print $1}'"], text=True
).strip()
print("IP", ip)
ready = threading.Event()
port = 19999


def srv():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((ip, port))
    ready.set()
    s.settimeout(2)
    try:
        data, addr = s.recvfrom(100)
        print("PY_RECV", data, addr)
    except Exception as e:
        print("PY_FAIL", e)
    s.close()


t = threading.Thread(target=srv)
t.start()
ready.wait()
c = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
n = c.sendto(b"ping", (ip, port))
print("sent", n, "to", ip, port)
t.join()

# also try 0.0.0.0 bind + send to ip
ready2 = threading.Event()


def srv2():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", 20001))
    ready2.set()
    s.settimeout(2)
    try:
        data, addr = s.recvfrom(100)
        print("PY_RECV2", data, addr)
    except Exception as e:
        print("PY_FAIL2", e)
    s.close()


t2 = threading.Thread(target=srv2)
t2.start()
ready2.wait()
c2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
print("sent2", c2.sendto(b"ping2", (ip, 20001)))
print("sent2loop", c2.sendto(b"ping2b", ("127.0.0.1", 20001)))
t2.join()
