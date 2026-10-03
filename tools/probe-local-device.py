"""Discover HyGrow over UDP and fetch its read-only local sensor JSON."""
import argparse
import ipaddress
import json
import socket
import time
import urllib.request
import uuid

PORT = 39400

def get_status(ip, timeout):
    ip = str(ipaddress.IPv4Address(ip))
    request = urllib.request.Request(f"http://{ip}/status", headers={"Cache-Control": "no-cache"})
    # LAN traffic must not be routed through a machine's HTTP proxy.
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(request, timeout=timeout) as response:
        if response.status != 200 or response.headers.get_content_type() != "application/json":
            raise ValueError("Device did not return HTTP 200 application/json")
        body = response.read(16385)
        if len(body) > 16384:
            raise ValueError("Status response too large")
        status = json.loads(body)
    if not isinstance(status, dict) or status.get("schemaVersion") != 1:
        raise ValueError("Unsupported telemetry schema")
    return status

def discover(broadcast, bind, device_id, timeout):
    nonce = uuid.uuid4().hex
    request = {"type": "hygrow_discover", "version": 1, "requestId": nonce}
    if device_id:
        request["deviceId"] = device_id
    body = json.dumps(request, separators=(",", ":")).encode()
    if len(body) > 256:
        raise ValueError("Discovery request exceeds 256 bytes")
    found = {}
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.bind((bind, 0))
        start = time.monotonic()
        next_send = start
        sends = 0
        deadline = start + timeout
        while time.monotonic() < deadline:
            now = time.monotonic()
            if sends < 3 and now >= next_send:
                sock.sendto(body, (str(ipaddress.IPv4Address(broadcast)), PORT))
                sends += 1
                next_send += 0.5
            sock.settimeout(min(0.2, max(0.01, deadline - now)))
            try:
                data, source = sock.recvfrom(2048)
            except socket.timeout:
                continue
            try:
                reply = json.loads(data)
                if not isinstance(reply, dict):
                    continue
                if (source[1] != PORT or reply.get("type") != "hygrow_announce" or
                    reply.get("version") != 1 or reply.get("requestId") != nonce or
                    reply.get("ip") != source[0] or reply.get("port") != 80 or
                    reply.get("statusPath") != "/status" or not reply.get("hardwareId")):
                    continue
                if device_id and reply.get("deviceId") != device_id:
                    continue
                found[reply["hardwareId"]] = reply
            except (ValueError, TypeError):
                continue
    return list(found.values())

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--broadcast", default="255.255.255.255", help="Wi-Fi subnet broadcast, e.g. 192.168.0.255")
    parser.add_argument("--bind", default="0.0.0.0", help="This computer's Wi-Fi IPv4 address for machines with multiple interfaces")
    parser.add_argument("--device-id", help="Only return this configured device ID")
    parser.add_argument("--ip", help="Fetch /status directly when an IP is already known")
    parser.add_argument("--timeout", type=float, default=2.0)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if args.device_id and not 1 <= len(args.device_id) <= 31:
        parser.error("--device-id must contain 1 to 31 characters")
    if args.ip:
        print(json.dumps(get_status(args.ip, args.timeout), indent=2))
        return
    devices = discover(args.broadcast, args.bind, args.device_id, args.timeout)
    if not devices:
        raise SystemExit("No HyGrow reply. Check shared Wi-Fi, client isolation, firewall, broadcast address and local permissions.")
    for device in devices:
        status = get_status(device["ip"], args.timeout)
        if status.get("deviceId") != device["deviceId"] or status.get("hardwareId") != device["hardwareId"]:
            raise SystemExit("Discovery/status identity mismatch; rediscover before using this endpoint.")
        print(json.dumps({"discovery": device, "status": status}, indent=2))

if __name__ == "__main__":
    main()
