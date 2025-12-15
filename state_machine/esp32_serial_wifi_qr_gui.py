import os
import re
import ssl
import time
import json
import hashlib
import threading
from dataclasses import dataclass
from typing import Optional, List

import serial
import serial.tools.list_ports

import tkinter as tk
from tkinter import ttk, filedialog, messagebox

import qrcode
from PIL import ImageTk

from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
from functools import partial

import json
from urllib.parse import urlparse



# -----------------------------
# Parsing + cache (Serial)
# -----------------------------
SSID_RE = re.compile(r"\[wifiConect\]\s*AP SSID:\s*(.+)\s*$", re.IGNORECASE)
PASS_RE = re.compile(r"\[wifiConect\]\s*AP PASS:\s*(.+)\s*$", re.IGNORECASE)
IP_RE   = re.compile(r"\[wifiConect\]\s*AP IP:\s*(\d+\.\d+\.\d+\.\d+)\s*$", re.IGNORECASE)

# Acepta PIN tanto del módulo WiFi como de la capa de comandos
PIN_RE  = re.compile(r"\[(?:wifiConect|CMD)\]\s*PIN portal:\s*(\d+)\s*$", re.IGNORECASE)

OPEN_TOKENS = {"(open)", "open", "OPEN", ""}

OTA_TRIGGER = {
    "enabled": False,
    "manifest_path": "/firmware/manifest.json",
    "update_id": 0
}


def escape_wifi_qr(s: str) -> str:
    return s.replace("\\", "\\\\").replace(";", r"\;").replace(",", r"\,").replace(":", r"\:")


@dataclass
class WifiPortalInfo:
    ssid: Optional[str] = None
    password: Optional[str] = None
    ip: Optional[str] = None
    pin: Optional[str] = None
    last_update_ts: float = 0.0

    def wifi_qr_payload(self) -> Optional[str]:
        if not self.ssid or self.password is None:
            return None

        ssid = escape_wifi_qr(self.ssid)
        pwd = escape_wifi_qr(self.password)

        if self.password.strip() in OPEN_TOKENS:
            return f"WIFI:T:nopass;S:{ssid};P:;H:false;;"
        return f"WIFI:T:WPA;S:{ssid};P:{pwd};H:false;;"


# -----------------------------
# Serial reader (robusto Windows + no-freeze close)
# -----------------------------
class SerialReader:
    def __init__(self):
        self._ser: Optional[serial.Serial] = None
        self._thread: Optional[threading.Thread] = None
        self._stop = threading.Event()

        self._last_port: Optional[str] = None
        self._last_baud: int = 115200

        self.cache: List[str] = []
        self.info = WifiPortalInfo()

        self.on_line = None
        self.on_info = None
        self.on_status = None

        self._tx_lock = threading.Lock()

    def open(self, port: str, baud: int = 115200):
        self.close()

        self._stop.clear()
        self._last_port = port
        self._last_baud = baud

        # Non-blocking para evitar hangs en Windows
        self._ser = serial.Serial(port=port, baudrate=baud, timeout=0)

        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def close(self):
        # No bloquea UI: cierra el puerto en background
        self._stop.set()

        ser = self._ser
        self._ser = None
        self._thread = None

        def _do_close(s):
            if not s:
                return
            try:
                try:
                    s.cancel_read()
                except Exception:
                    pass
                try:
                    s.cancel_write()
                except Exception:
                    pass
                s.close()
            except Exception:
                pass

        threading.Thread(target=_do_close, args=(ser,), daemon=True).start()

    def is_open(self) -> bool:
        return bool(self._ser and self._ser.is_open)

    def send_line(self, line: str):
        if self._stop.is_set():
            return
        if not line:
            return
        if not line.endswith("\n"):
            line = line + "\n"

        with self._tx_lock:
            s = self._ser
            if s and s.is_open:
                try:
                    s.write(line.encode("utf-8", errors="ignore"))
                    # NO flush(): puede colgar si el dispositivo se resetea
                except Exception as e:
                    self._handle_line(f"[PY] TX error: {e}")

    def _status(self, msg: str):
        if self.on_status:
            self.on_status(msg)
        self._handle_line(f"[PY] {msg}")

    def _run(self):
        buf = b""
        while not self._stop.is_set():
            try:
                s = self._ser
                if not s or not s.is_open:
                    time.sleep(0.05)
                    continue

                chunk = s.read(256)
                if not chunk:
                    time.sleep(0.02)
                    continue

                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    line = line.replace(b"\r", b"")
                    text = line.decode("utf-8", errors="replace")
                    self._handle_line(text)

            except Exception as e:
                if self._stop.is_set():
                    break
                self._status(f"Serial error: {e}")
                self._safe_reset_serial()
                self._auto_reconnect()
                time.sleep(0.2)

    def _safe_reset_serial(self):
        try:
            if self._ser:
                try:
                    self._ser.close()
                except Exception:
                    pass
        except Exception:
            pass
        self._ser = None

    def _auto_reconnect(self):
        if self._stop.is_set():
            return

        time.sleep(0.8)

        # Reintenta mismo COM
        if self._last_port:
            try:
                self._ser = serial.Serial(port=self._last_port, baudrate=self._last_baud, timeout=0)
                self._status(f"Reconnected to {self._last_port} @ {self._last_baud}")
                return
            except Exception as e2:
                self._status(f"Reconnect failed on same port: {e2}")

        # Fallback: primer puerto disponible
        ports = [p.device for p in serial.tools.list_ports.comports()]
        if ports:
            try:
                self._last_port = ports[0]
                self._ser = serial.Serial(port=self._last_port, baudrate=self._last_baud, timeout=0)
                self._status(f"Reconnected to {self._last_port} @ {self._last_baud} (fallback)")
            except Exception as e3:
                self._status(f"Reconnect fallback failed: {e3}")

    def _handle_line(self, line: str):
        self.cache.append(line)
        if self.on_line:
            self.on_line(line)

        updated = False

        m = SSID_RE.search(line)
        if m:
            self.info.ssid = m.group(1).strip()
            updated = True

        m = PASS_RE.search(line)
        if m:
            self.info.password = m.group(1).strip()
            updated = True

        m = IP_RE.search(line)
        if m:
            self.info.ip = m.group(1).strip()
            updated = True

        m = PIN_RE.search(line)
        if m:
            self.info.pin = m.group(1).strip()
            updated = True

        if updated:
            self.info.last_update_ts = time.time()
            if self.on_info:
                self.on_info(self.info)



class OtaRequestHandler(SimpleHTTPRequestHandler):
    def do_GET(self):
        # ===== OTA trigger endpoint =====
        if self.path.startswith("/api/update-check"):
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()

            if OTA_TRIGGER["enabled"]:
                payload = {
                    "update": True,
                    "id": OTA_TRIGGER["update_id"],
                    "manifest": OTA_TRIGGER["manifest_path"]
                }
            else:
                payload = {
                    "update": False,
                    "id": OTA_TRIGGER["update_id"]
                }

            self.wfile.write(json.dumps(payload).encode("utf-8"))
            return

        # fallback: sirve archivos estáticos normal
        return super().do_GET()



# -----------------------------
# HTTPS Server (SimpleHTTPRequestHandler + SSL)
# -----------------------------
class HttpsServerController:
    def __init__(self):
        self._httpd: Optional[ThreadingHTTPServer] = None
        self._thread: Optional[threading.Thread] = None

        self.root_dir: str = ""
        self.host: str = "0.0.0.0"
        self.port: int = 8443
        self.cert_path: str = ""
        self.key_path: str = ""

    def is_running(self) -> bool:
        return self._httpd is not None

    def start(self, root_dir: str, host: str, port: int, cert_path: str, key_path: str):
        if self.is_running():
            raise RuntimeError("Server already running")

        if not os.path.isdir(root_dir):
            raise RuntimeError("root_dir inválido")

        if not os.path.isfile(cert_path):
            raise RuntimeError("cert_path inválido (server.crt)")
        if not os.path.isfile(key_path):
            raise RuntimeError("key_path inválido (server.key)")

        self.root_dir = root_dir
        self.host = host
        self.port = port
        self.cert_path = cert_path
        self.key_path = key_path

        handler = partial(OtaRequestHandler, directory=root_dir)
        httpd = ThreadingHTTPServer((host, port), handler)

        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(certfile=cert_path, keyfile=key_path)

        httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)

        self._httpd = httpd
        self._thread = threading.Thread(target=httpd.serve_forever, daemon=True)
        self._thread.start()
        
    def stop(self):
        httpd = self._httpd
        self._httpd = None

        if httpd:
            try:
                httpd.shutdown()
            except Exception:
                pass
            try:
                httpd.server_close()
            except Exception:
                pass


# -----------------------------
# GUI
# -----------------------------
class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("ESP32 - Serial Cache + WiFi QR + HTTPS FW Server")
        self.geometry("1180x720")

        self.reader = SerialReader()
        self.reader.on_line = self._on_serial_line
        self.reader.on_info = self._on_portal_info
        self.reader.on_status = self._on_status

        self.https = HttpsServerController()

        self._qr_photo = None

        self._build_ui()
        self._refresh_ports()

        self.protocol("WM_DELETE_WINDOW", self._on_close)

    def _build_ui(self):
        root = ttk.Frame(self, padding=10)
        root.pack(fill="both", expand=True)

        self.tabs = ttk.Notebook(root)
        self.tabs.pack(fill="both", expand=True)

        # Tab 1: Serial + QR
        tab1 = ttk.Frame(self.tabs)
        self.tabs.add(tab1, text="Serial + QR")

        # Tab 2: HTTPS Server
        tab2 = ttk.Frame(self.tabs)
        self.tabs.add(tab2, text="HTTPS Server")

        self._build_tab_serial(tab1)
        self._build_tab_https(tab2)

    # ---------- TAB 1 ----------
    def _build_tab_serial(self, parent):
        top = ttk.Frame(parent)
        top.pack(fill="x")

        ttk.Label(top, text="Puerto:").pack(side="left")
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(top, textvariable=self.port_var, width=18, state="readonly")
        self.port_combo.pack(side="left", padx=6)

        ttk.Label(top, text="Baud:").pack(side="left")
        self.baud_var = tk.StringVar(value="115200")
        ttk.Entry(top, textvariable=self.baud_var, width=10).pack(side="left", padx=6)

        ttk.Button(top, text="Refresh", command=self._refresh_ports).pack(side="left", padx=6)

        self.btn_connect = ttk.Button(top, text="Conectar", command=self._toggle_connection)
        self.btn_connect.pack(side="left", padx=6)
        
        ttk.Button(top, text="Clear Console", command=self._clear_console).pack(side="left", padx=4)


        ttk.Button(top, text="Guardar log...", command=self._save_log).pack(side="right")

        self.status_var = tk.StringVar(value="Desconectado")
        ttk.Label(top, textvariable=self.status_var).pack(side="right", padx=12)

        mid = ttk.Frame(parent)
        mid.pack(fill="both", expand=True, pady=10)

        left = ttk.Frame(mid)
        left.pack(side="left", fill="both", expand=True)

        right = ttk.Frame(mid, width=420)
        right.pack(side="right", fill="y")
        right.pack_propagate(False)

        ttk.Label(left, text="Serial log (cache):").pack(anchor="w")
        self.log_text = tk.Text(left, height=24, wrap="none")
        self.log_text.pack(fill="both", expand=True)
        self.log_text.configure(state="disabled")

        cmd_panel = ttk.LabelFrame(left, text="Comandos (USB)")
        cmd_panel.pack(fill="x", pady=8)

        row1 = ttk.Frame(cmd_panel)
        row1.pack(fill="x", pady=4)
        ttk.Button(row1, text="WIFI_RESET", command=lambda: self._send_cmd("WIFI_RESET")).pack(side="left", padx=4)
        ttk.Button(row1, text="WIFI_PORTAL", command=lambda: self._send_cmd("WIFI_PORTAL")).pack(side="left", padx=4)
        ttk.Button(row1, text="WIFI_STATUS", command=lambda: self._send_cmd("WIFI_STATUS")).pack(side="left", padx=4)
        ttk.Button(row1, text="WIFI_DIAG", command=lambda: self._send_cmd("WIFI_DIAG")).pack(side="left", padx=4)
        ttk.Button(row1, text="TIME_GET", command=lambda: self._send_cmd("TIME_GET")).pack(side="left", padx=4)

        row2 = ttk.Frame(cmd_panel)
        row2.pack(fill="x", pady=4)
        ttk.Label(row2, text="TZ_SET").pack(side="left", padx=4)
        self.tz_var = tk.StringVar(value="EST5EDT,M3.2.0/2,M11.1.0/2")
        ttk.Entry(row2, textvariable=self.tz_var, width=40).pack(side="left", padx=4)
        ttk.Button(row2, text="Enviar", command=self._send_tz).pack(side="left", padx=4)

        row3 = ttk.Frame(cmd_panel)
        row3.pack(fill="x", pady=4)
        ttk.Button(row3, text="PIN_GET", command=lambda: self._send_cmd("PIN_GET")).pack(side="left", padx=4)
        ttk.Button(row3, text="PIN_NEW", command=lambda: self._send_cmd("PIN_NEW")).pack(side="left", padx=4)

        row4 = ttk.Frame(cmd_panel)
        row4.pack(fill="x", pady=4)
        ttk.Label(row4, text="Custom:").pack(side="left", padx=4)
        self.custom_cmd_var = tk.StringVar()
        ent = ttk.Entry(row4, textvariable=self.custom_cmd_var)
        ent.pack(side="left", fill="x", expand=True, padx=4)
        ent.bind("<Return>", lambda e: self._send_custom())
        ttk.Button(row4, text="Enviar", command=self._send_custom).pack(side="left", padx=4)

        # Right panel: portal info + QR
        ttk.Label(right, text="Portal info detectado:").pack(anchor="w")

        form = ttk.Frame(right)
        form.pack(fill="x", pady=6)

        self.ssid_val = tk.StringVar(value="-")
        self.pass_val = tk.StringVar(value="-")
        self.ip_val   = tk.StringVar(value="-")
        self.pin_val  = tk.StringVar(value="-")

        self._kv(form, "SSID", self.ssid_val)
        self._kv(form, "PASS", self.pass_val)
        self._kv(form, "IP",   self.ip_val)
        self._kv(form, "PIN",  self.pin_val)

        ttk.Separator(right).pack(fill="x", pady=8)

        ttk.Label(right, text="QR para conectar WiFi:").pack(anchor="w")
        self.qr_label = ttk.Label(right)
        self.qr_label.pack(pady=8)

        self.qr_payload_var = tk.StringVar(value="(esperando SSID/PASS desde Serial)")
        ttk.Label(right, textvariable=self.qr_payload_var, wraplength=380).pack(anchor="w", pady=6)

        btns = ttk.Frame(right)
        btns.pack(fill="x", pady=6)
        ttk.Button(btns, text="Copiar payload", command=self._copy_payload).pack(side="left")
        ttk.Button(btns, text="Portal (IP)", command=self._open_portal_hint).pack(side="right")

    # ---------- TAB 2 ----------
    def _build_tab_https(self, parent):
        frame = ttk.Frame(parent, padding=10)
        frame.pack(fill="both", expand=True)

        # Root dir
        row0 = ttk.LabelFrame(frame, text="Directorio del servidor")
        row0.pack(fill="x", pady=6)

        self.srv_root_var = tk.StringVar(value=os.path.abspath("./fw_server_root"))
        ttk.Entry(row0, textvariable=self.srv_root_var).pack(side="left", fill="x", expand=True, padx=6, pady=6)
        ttk.Button(row0, text="Seleccionar...", command=self._pick_root_dir).pack(side="left", padx=6, pady=6)
        ttk.Button(row0, text="Crear estructura", command=self._create_server_structure).pack(side="left", padx=6, pady=6)

        # Bin selection
        row1 = ttk.LabelFrame(frame, text="Firmware")
        row1.pack(fill="x", pady=6)

        self.bin_path_var = tk.StringVar(value="")
        ttk.Entry(row1, textvariable=self.bin_path_var).pack(side="left", fill="x", expand=True, padx=6, pady=6)
        ttk.Button(row1, text="Seleccionar .bin...", command=self._pick_bin).pack(side="left", padx=6, pady=6)
        ttk.Button(row1, text="Copiar a /firmware/app.bin", command=self._copy_bin_to_server).pack(side="left", padx=6, pady=6)

        # Manifest info
        row2 = ttk.LabelFrame(frame, text="Manifest (size + sha256)")
        row2.pack(fill="x", pady=6)

        self.size_var = tk.StringVar(value="-")
        self.sha_var = tk.StringVar(value="-")
        ttk.Label(row2, text="Size:").pack(side="left", padx=6)
        ttk.Label(row2, textvariable=self.size_var, width=14).pack(side="left")
        ttk.Label(row2, text="SHA256:").pack(side="left", padx=6)
        ttk.Entry(row2, textvariable=self.sha_var, width=70).pack(side="left", padx=6, pady=6)
        ttk.Button(row2, text="Calcular", command=self._calc_hash).pack(side="left", padx=6, pady=6)
        ttk.Button(row2, text="Generar manifest.json", command=self._write_manifest).pack(side="left", padx=6, pady=6)

        # TLS cert/key
        row3 = ttk.LabelFrame(frame, text="TLS (certificado autofirmado)")
        row3.pack(fill="x", pady=6)

        self.cert_var = tk.StringVar(value=os.path.abspath("./server.crt"))
        self.key_var  = tk.StringVar(value=os.path.abspath("./server.key"))

        ttk.Label(row3, text="server.crt:").grid(row=0, column=0, sticky="w", padx=6, pady=4)
        ttk.Entry(row3, textvariable=self.cert_var).grid(row=0, column=1, sticky="ew", padx=6, pady=4)
        ttk.Button(row3, text="...", command=self._pick_cert).grid(row=0, column=2, padx=6, pady=4)

        ttk.Label(row3, text="server.key:").grid(row=1, column=0, sticky="w", padx=6, pady=4)
        ttk.Entry(row3, textvariable=self.key_var).grid(row=1, column=1, sticky="ew", padx=6, pady=4)
        ttk.Button(row3, text="...", command=self._pick_key).grid(row=1, column=2, padx=6, pady=4)

        row3.columnconfigure(1, weight=1)

        # Server controls
        row4 = ttk.LabelFrame(frame, text="Servidor HTTPS")
        row4.pack(fill="x", pady=6)

        self.host_var = tk.StringVar(value="0.0.0.0")
        self.port_var2 = tk.StringVar(value="8443")

        ttk.Label(row4, text="Host:").pack(side="left", padx=6)
        ttk.Entry(row4, textvariable=self.host_var, width=12).pack(side="left")
        ttk.Label(row4, text="Port:").pack(side="left", padx=6)
        ttk.Entry(row4, textvariable=self.port_var2, width=8).pack(side="left")

        self.btn_srv = ttk.Button(row4, text="Iniciar servidor", command=self._toggle_server)
        self.btn_srv.pack(side="left", padx=10)

        self.srv_status_var = tk.StringVar(value="Detenido")
        ttk.Label(row4, textvariable=self.srv_status_var).pack(side="left", padx=12)
        
        row_trigger = ttk.LabelFrame(frame, text="OTA Trigger (HTTPS)")
        row_trigger.pack(fill="x", padx=6, pady=6)
        
        self.ota_trigger_var = tk.BooleanVar(value=False)
        
        ttk.Checkbutton(
            row_trigger,
            text="Habilitar trigger de OTA (update-check = True)",
            variable=self.ota_trigger_var,
            command=self._ui_toggle_ota_trigger
        ).pack(side="left", padx=6, pady=6)
        
        ttk.Button(
            row_trigger,
            text="Enviar FW_PULL por USB (opcional)",
            command=lambda: self._send_cmd("FW_PULL")
        ).pack(side="left", padx=6, pady=6)


        # URLs
        row5 = ttk.LabelFrame(frame, text="URLs para ESP32 (cliente HTTPS)")
        row5.pack(fill="x", pady=6)

        self.url_bin_var = tk.StringVar(value="-")
        self.url_manifest_var = tk.StringVar(value="-")

        ttk.Label(row5, text="BIN:").grid(row=0, column=0, sticky="w", padx=6, pady=4)
        ttk.Entry(row5, textvariable=self.url_bin_var).grid(row=0, column=1, sticky="ew", padx=6, pady=4)
        ttk.Button(row5, text="Copiar", command=lambda: self._copy_text(self.url_bin_var.get())).grid(row=0, column=2, padx=6, pady=4)

        ttk.Label(row5, text="Manifest:").grid(row=1, column=0, sticky="w", padx=6, pady=4)
        ttk.Entry(row5, textvariable=self.url_manifest_var).grid(row=1, column=1, sticky="ew", padx=6, pady=4)
        ttk.Button(row5, text="Copiar", command=lambda: self._copy_text(self.url_manifest_var.get())).grid(row=1, column=2, padx=6, pady=4)

        row5.columnconfigure(1, weight=1)
        
        
        # Disparo por USB 
        row6 = ttk.LabelFrame(frame, text="Disparo por USB (ESP32)")
        row6.pack(fill="x", pady=6)
        
        ttk.Button(row6, text="Enviar FW_URL_SET (manifest)", command=self._send_fw_url_set).pack(side="left", padx=6, pady=6)
        ttk.Button(row6, text="Enviar FW_PULL", command=lambda: self._send_cmd("FW_PULL")).pack(side="left", padx=6, pady=6)
        ttk.Button(row6, text="FW_STATUS", command=lambda: self._send_cmd("FW_STATUS")).pack(side="left", padx=6, pady=6)


        # Tips
        tips = ttk.LabelFrame(frame, text="Notas")
        tips.pack(fill="both", expand=True, pady=6)

        txt = (
            "1) Crea un certificado autofirmado (OpenSSL) y apunta server.crt/server.key.\n"
            "   openssl req -x509 -newkey rsa:2048 -sha256 -days 3650 -nodes "
            "-keyout server.key -out server.crt -subj \"/CN=esp32-fw-server\"\n\n"
            "2) Copia el .bin a /firmware/app.bin y genera manifest.json.\n"
            "3) En el ESP32, pega el server.crt como PEM (CA) y usa la URL mostrada.\n"
        )
        ttk.Label(tips, text=txt, justify="left").pack(anchor="w", padx=8, pady=8)

    # ---------- Common helpers ----------
    def _kv(self, parent, key, var):
        row = ttk.Frame(parent)
        row.pack(fill="x", pady=2)
        ttk.Label(row, text=f"{key}:", width=7).pack(side="left")
        ttk.Label(row, textvariable=var).pack(side="left")

    def _refresh_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.port_combo["values"] = ports
        if ports and (not self.port_var.get() or self.port_var.get() not in ports):
            self.port_var.set(ports[0])

    def _toggle_connection(self):
        if self.reader.is_open():
            self.reader.close()
            self.btn_connect.configure(text="Conectar")
            self.status_var.set("Desconectado")
            return

        port = self.port_var.get()
        if not port:
            messagebox.showerror("Error", "Selecciona un puerto serial.")
            return

        try:
            baud = int(self.baud_var.get().strip())
        except ValueError:
            messagebox.showerror("Error", "Baud inválido.")
            return

        try:
            self.reader.open(port, baud)
            self.btn_connect.configure(text="Desconectar")
            self.status_var.set(f"Conectado a {port}")
            self._append_log(f"[PY] Conectado a {port} @ {baud}\n")
        except Exception as e:
            messagebox.showerror("Error", f"No se pudo abrir el puerto: {e}")

    def _send_cmd(self, cmd: str):
        if not self.reader.is_open():
            messagebox.showwarning("Serial", "No estás conectado al puerto.")
            return
        self.reader.send_line(cmd)

    def _send_tz(self):
        tz = self.tz_var.get().strip()
        if not tz:
            return
        self._send_cmd(f"TZ_SET {tz}")

    def _send_custom(self):
        cmd = self.custom_cmd_var.get().strip()
        if not cmd:
            return
        self._send_cmd(cmd)
        self.custom_cmd_var.set("")

    def _on_serial_line(self, line: str):
        self.after(0, lambda: self._append_log(line + "\n"))

    def _on_portal_info(self, info: WifiPortalInfo):
        self.after(0, lambda: self._update_info(info))

    def _on_status(self, status: str):
        self.after(0, lambda: self.status_var.set(status))

    def _append_log(self, text: str):
        self.log_text.configure(state="normal")
        self.log_text.insert("end", text)
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    def _update_info(self, info: WifiPortalInfo):
        self.ssid_val.set(info.ssid or "-")
        self.pass_val.set(info.password or "-")
        self.ip_val.set(info.ip or "-")
        self.pin_val.set(info.pin or "-")

        payload = info.wifi_qr_payload()
        if not payload:
            self.qr_payload_var.set("(esperando SSID/PASS desde Serial)")
            self.qr_label.configure(image="")
            self._qr_photo = None
            return

        self.qr_payload_var.set(payload)
        self._render_qr(payload)

    def _render_qr(self, payload: str):
        qr = qrcode.QRCode(border=2, box_size=6)
        qr.add_data(payload)
        qr.make(fit=True)
        img = qr.make_image(fill_color="black", back_color="white").convert("RGB")
        img = img.resize((320, 320))
        self._qr_photo = ImageTk.PhotoImage(img)
        self.qr_label.configure(image=self._qr_photo)

    def _copy_payload(self):
        text = self.qr_payload_var.get()
        if not text or text.startswith("("):
            return
        self._copy_text(text)

    def _copy_text(self, text: str):
        if not text or text == "-":
            return
        self.clipboard_clear()
        self.clipboard_append(text)

    def _open_portal_hint(self):
        ip = self.ip_val.get()
        if ip == "-" or not ip:
            ip = "192.168.4.1"
        messagebox.showinfo("Portal cautivo", f"Conéctate al WiFi del ESP32 y abre:\nhttp://{ip}/")

    def _save_log(self):
        if not self.reader.cache:
            messagebox.showinfo("Guardar log", "No hay datos aún.")
            return
        path = filedialog.asksaveasfilename(
            defaultextension=".txt",
            filetypes=[("Text file", "*.txt"), ("All files", "*.*")]
        )
        if not path:
            return
        with open(path, "w", encoding="utf-8") as f:
            for line in self.reader.cache:
                f.write(line + "\n")
        messagebox.showinfo("Guardar log", f"Log guardado en:\n{path}")

    # ---------- HTTPS tab actions ----------
    def _pick_root_dir(self):
        p = filedialog.askdirectory()
        if p:
            self.srv_root_var.set(p)

    def _create_server_structure(self):
        root = self.srv_root_var.get().strip()
        if not root:
            return
        fw_dir = os.path.join(root, "firmware")
        os.makedirs(fw_dir, exist_ok=True)
        messagebox.showinfo("OK", f"Estructura creada:\n{fw_dir}")

    def _pick_bin(self):
        p = filedialog.askopenfilename(filetypes=[("Firmware bin", "*.bin"), ("All files", "*.*")])
        if p:
            self.bin_path_var.set(p)

    def _copy_bin_to_server(self):
        root = self.srv_root_var.get().strip()
        bin_path = self.bin_path_var.get().strip()
        if not root or not bin_path:
            messagebox.showerror("Error", "Selecciona root y .bin primero.")
            return
        fw_dir = os.path.join(root, "firmware")
        os.makedirs(fw_dir, exist_ok=True)

        dst = os.path.join(fw_dir, "app.bin")
        try:
            with open(bin_path, "rb") as src_f, open(dst, "wb") as dst_f:
                while True:
                    chunk = src_f.read(1024 * 1024)
                    if not chunk:
                        break
                    dst_f.write(chunk)
        except Exception as e:
            messagebox.showerror("Error", f"No se pudo copiar:\n{e}")
            return

        messagebox.showinfo("OK", f"Copiado a:\n{dst}")
        self._calc_hash()  # recalcular sobre el bin fuente (o puedes recalcular sobre el dst)

    def _calc_hash(self):
        bin_path = self.bin_path_var.get().strip()
        if not bin_path or not os.path.isfile(bin_path):
            messagebox.showerror("Error", "Selecciona un archivo .bin válido.")
            return

        try:
            h = hashlib.sha256()
            size = 0
            with open(bin_path, "rb") as f:
                while True:
                    b = f.read(1024 * 1024)
                    if not b:
                        break
                    h.update(b)
                    size += len(b)
            self.size_var.set(str(size))
            self.sha_var.set(h.hexdigest())
        except Exception as e:
            messagebox.showerror("Error", f"No se pudo calcular hash:\n{e}")

    def _write_manifest(self):
        root = self.srv_root_var.get().strip()
        if not root:
            messagebox.showerror("Error", "Selecciona root del servidor.")
            return

        size_txt = self.size_var.get().strip()
        sha = self.sha_var.get().strip()

        if not size_txt.isdigit() or len(sha) != 64:
            messagebox.showerror("Error", "Primero calcula size + SHA256 (Calcular).")
            return

        fw_dir = os.path.join(root, "firmware")
        os.makedirs(fw_dir, exist_ok=True)

        manifest = {
            "file": "/firmware/app.bin",
            "size": int(size_txt),
            "sha256": sha
        }

        out = os.path.join(fw_dir, "manifest.json")
        try:
            with open(out, "w", encoding="utf-8") as f:
                json.dump(manifest, f, indent=2)
        except Exception as e:
            messagebox.showerror("Error", f"No se pudo escribir manifest:\n{e}")
            return

        messagebox.showinfo("OK", f"Manifest generado:\n{out}")

    def _pick_cert(self):
        p = filedialog.askopenfilename(filetypes=[("Certificate", "*.crt *.pem"), ("All files", "*.*")])
        if p:
            self.cert_var.set(p)

    def _pick_key(self):
        p = filedialog.askopenfilename(filetypes=[("Private key", "*.key *.pem"), ("All files", "*.*")])
        if p:
            self.key_var.set(p)

    def _toggle_server(self):
        if self.https.is_running():
            self.https.stop()
            self.btn_srv.configure(text="Iniciar servidor")
            self.srv_status_var.set("Detenido")
            return

        root = self.srv_root_var.get().strip()
        cert = self.cert_var.get().strip()
        key = self.key_var.get().strip()
        host = self.host_var.get().strip() or "0.0.0.0"

        try:
            port = int(self.port_var2.get().strip())
        except ValueError:
            messagebox.showerror("Error", "Port inválido.")
            return

        try:
            self.https.start(root_dir=root, host=host, port=port, cert_path=cert, key_path=key)
        except Exception as e:
            messagebox.showerror("Error", f"No se pudo iniciar servidor:\n{e}")
            return

        self.btn_srv.configure(text="Detener servidor")
        self.srv_status_var.set(f"Corriendo en {host}:{port}")

        # Construir URLs (usa IP local si la tienes)
        # Nota: Para ESP32, pon aquí la IP real de tu laptop (misma red).
        ip_hint = self._best_local_ip_hint()
        self.url_bin_var.set(f"https://{ip_hint}:{port}/firmware/app.bin")
        self.url_manifest_var.set(f"https://{ip_hint}:{port}/firmware/manifest.json")

    def _best_local_ip_hint(self) -> str:
        # Heurística simple: si ya viste IP STA del ESP32 en logs, no sirve aquí.
        # Para evitar dependencias, dejamos placeholder y que el usuario reemplace.
        # Puedes poner manualmente tu IP local en el campo URL luego.
        return "192.168.1.190"
        
    def _send_fw_url_set(self):
        if not self.reader.is_open():
            messagebox.showwarning("Serial", "No estás conectado al puerto.")
            return
        url = self.url_manifest_var.get().strip()
        if not url or url == "-" or "YOUR_LAPTOP_IP" in url:
            messagebox.showwarning("URL", "URL de manifest inválida. Ajusta la IP real.")
            return
        self.reader.send_line(f"FW_URL_SET {url}")
        
    def _ui_toggle_ota_trigger(self):
        OTA_TRIGGER["enabled"] = bool(self.ota_trigger_var.get())
        if OTA_TRIGGER["enabled"]:
            OTA_TRIGGER["update_id"] += 1
        self._append_log(f"[HTTPS] OTA trigger {'ENABLED' if OTA_TRIGGER['enabled'] else 'DISABLED'} id={OTA_TRIGGER['update_id']}\n")

    def _clear_console(self):
        # Limpia el Text widget visible
        self.log_text.configure(state="normal")
        self.log_text.delete("1.0", "end")
        self.log_text.configure(state="disabled")
    
        # (Opcional pero recomendado) Limpia cache interna para que "Guardar log..." no guarde lo viejo
        self.reader.cache.clear()
    
        # (Opcional) Log local de confirmación
        self._append_log("[PY] Console cleared.\n")


    def _on_close(self):
        try:
            self.https.stop()
        except Exception:
            pass
        try:
            self.reader.close()
        except Exception:
            pass
        self.destroy()


if __name__ == "__main__":
    app = App()
    app.mainloop()
