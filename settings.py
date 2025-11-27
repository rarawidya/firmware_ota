#!/usr/bin/env python3

import argparse
import requests
import json
import sys

def send_settings_to_esp32(device_name, wifi_mode, ap_ssid, ap_pass, sta_ssid, sta_pass, ip=None, port=None):
    """
    Send settings to ESP32 device via HTTP API

    Args:
        device_name (str): Device name
        wifi_mode (str): Wi-Fi mode ('ap' or 'sta')
        ap_ssid (str): AP SSID
        ap_pass (str): AP Password
        sta_ssid (str): Station SSID
        sta_pass (str): Station Password
        ip (str): ESP32 IP address or hostname (default: 192.168.4.1)
        port (int): ESP32 port (default: 80)
    """

    if not ip or ip == "":
        ip = "192.168.4.1"
    else:
        if ip.startswith("http://"):
            ip = ip[7:]  
        elif ip.startswith("https://"):
            ip = ip[8:]  

        if ip.endswith("/"):
            ip = ip[:-1]

    if not port or port == "" or port is None:
        port = 80
    else:
        try:
            port = int(port)
        except ValueError:
            print(f"Warning: Port '{port}' is not a valid integer. Using default: 80")
            port = 80

    if wifi_mode not in ['ap', 'sta']:
        raise ValueError("Wi-Fi mode must be either 'ap' or 'sta'")

    if wifi_mode == 'ap':
        if not sta_ssid or sta_ssid == "":
            sta_ssid = ""
        if not sta_pass or sta_pass == "":
            sta_pass = ""

    payload = {
        "deviceName": device_name,
        "wifiMode": wifi_mode,
        "apSsid": ap_ssid,
        "apPass": ap_pass,
        "staSsid": sta_ssid,
        "staPass": sta_pass
    }

    url = f"http://{ip}:{port}/settings"

    print(f"Sending settings to {url}")
    print(f"Payload: {json.dumps(payload, indent=2)}")

    try:
        response = requests.post(
            url,
            json=payload,
            headers={'Content-Type': 'application/json'},
            timeout=10
        )

        if response.status_code == 200:
            print(f"Settings sent successfully! Response: {response.text}")
            return True
        else:
            print(f"Failed to send settings. Status code: {response.status_code}, Response: {response.text}")
            return False

    except requests.exceptions.ConnectionError:
        print(f"Error: Could not connect to {url}. Please check if the ESP32 is accessible.")
        return False
    except requests.exceptions.Timeout:
        print(f"Error: Request timed out while connecting to {url}")
        return False
    except Exception as e:
        print(f"Unexpected error: {str(e)}")
        return False

def main():
    parser = argparse.ArgumentParser(description="Send settings to ESP32 OTA device")
    parser.add_argument("-d", "--device", required=True, help="Device name")
    parser.add_argument("-m", "--mode", required=True, choices=['ap', 'sta'],
                       help="Wi-Fi mode: 'ap' for Access Point, 'sta' for Station")
    parser.add_argument("-s", "--ssid", required=True,
                       help="When -m=ap: AP SSID; When -m=sta: Station SSID")
    parser.add_argument("-p", "--password", required=True,
                       help="When -m=ap: AP Password; When -m=sta: Station Password")
    parser.add_argument("-stssid", "--sta-ssid", required=False,
                       help="Station SSID (optional when -m=ap, required when -m=sta)")
    parser.add_argument("-stpwd", "--sta-pass", required=False,
                       help="Station Password (optional when -m=ap, required when -m=sta)")
    parser.add_argument("-i", "-ip", "--ip", default="", help="ESP32 IP address or hostname (default: 192.168.4.1 if empty)")
    parser.add_argument("--port", type=str, default="", help="ESP32 port (default: 80 if empty)")

    args = parser.parse_args()

    if args.mode == 'ap':
        ap_ssid = args.ssid
        ap_pass = args.password
        sta_ssid = args.sta_ssid if args.sta_ssid else ""
        sta_pass = args.sta_pass if args.sta_pass else ""
    else:
        sta_ssid = args.ssid
        sta_pass = args.password
        if args.sta_ssid is not None:
            sta_ssid = args.sta_ssid
        if args.sta_pass is not None:
            sta_pass = args.sta_pass
        ap_ssid = "ESP32-OTA"
        ap_pass = "esp32pass"

    success = send_settings_to_esp32(
        device_name=args.device,
        wifi_mode=args.mode,
        ap_ssid=ap_ssid,
        ap_pass=ap_pass,
        sta_ssid=sta_ssid,
        sta_pass=sta_pass,
        ip=args.ip,
        port=args.port
    )

    sys.exit(0 if success else 1)

if __name__ == "__main__":
    main()