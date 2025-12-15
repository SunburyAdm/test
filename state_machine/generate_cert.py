import os
import sys
import socket
import ipaddress
from datetime import datetime, timedelta

from cryptography import x509
from cryptography.x509.oid import NameOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa


CERT_FILE = "server.crt"
KEY_FILE  = "server.key"


def detect_local_ipv4(prefer_gateway_probe: str = "8.8.8.8", probe_port: int = 80) -> str:
    """
    Detecta la IP local (IPv4) más probable para conexiones salientes.
    Método: socket UDP "connect" (no envía datos) hacia un destino público.
    Funciona incluso sin respuesta del destino.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((prefer_gateway_probe, probe_port))
        ip = s.getsockname()[0]
    finally:
        s.close()

    # Validaciones básicas
    ip_obj = ipaddress.ip_address(ip)
    if ip_obj.version != 4:
        raise RuntimeError(f"Detecté IP no-IPv4: {ip}")
    if ip_obj.is_loopback or ip_obj.is_unspecified:
        raise RuntimeError(f"IP inválida detectada: {ip}")
    return ip


def pick_ip() -> str:
    """
    Prioridad:
    1) Argumento CLI: python generate_cert.py 192.168.1.100
    2) Variable de entorno LAPTOP_IP
    3) Auto-detección
    """
    if len(sys.argv) >= 2:
        return sys.argv[1].strip()

    env_ip = os.getenv("LAPTOP_IP", "").strip()
    if env_ip:
        return env_ip

    return detect_local_ipv4()


def main():
    laptop_ip = pick_ip()

    # Validar IP
    try:
        ip_obj = ipaddress.ip_address(laptop_ip)
        if ip_obj.version != 4:
            raise ValueError("Solo soporta IPv4 para SAN en este script.")
    except Exception as e:
        raise SystemExit(f"ERROR: IP inválida '{laptop_ip}': {e}")

    # Generar clave privada
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)

    # Subject / Issuer (autofirmado)
    subject = issuer = x509.Name([
        x509.NameAttribute(NameOID.COUNTRY_NAME, "MX"),
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, "ESP32 OTA Test"),
        x509.NameAttribute(NameOID.COMMON_NAME, "esp32-fw-server"),
    ])

    # SAN: IP + DNS
    san = x509.SubjectAlternativeName([
        x509.IPAddress(ip_obj),
        x509.DNSName("esp32-fw-server"),
    ])

    # Certificado (autofirmado)
    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(issuer)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(datetime.utcnow() - timedelta(days=1))
        .not_valid_after(datetime.utcnow() + timedelta(days=365))
        .add_extension(san, critical=False)
        .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
        .sign(key, hashes.SHA256())
    )

    # Guardar key
    with open(KEY_FILE, "wb") as f:
        f.write(
            key.private_bytes(
                encoding=serialization.Encoding.PEM,
                format=serialization.PrivateFormat.TraditionalOpenSSL,
                encryption_algorithm=serialization.NoEncryption(),
            )
        )

    # Guardar cert
    with open(CERT_FILE, "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))

    print("OK")
    print(f"Detected IP (SAN): {laptop_ip}")
    print(f"Certificate: {CERT_FILE}")
    print(f"Private key: {KEY_FILE}")
    print("Usage tips:")
    print("  - Override IP: python generate_cert.py 192.168.1.100")
    print("  - or set env:  set LAPTOP_IP=192.168.1.100  (Windows CMD)")
    print("                $env:LAPTOP_IP='192.168.1.100' (PowerShell)")


if __name__ == "__main__":
    main()
