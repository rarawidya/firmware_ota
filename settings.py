#!/usr/bin/env python3

import argparse
import requests
import json
import sys

def send_settings_to_esp32(device_name, wifi_mode, ap_ssid, ap_pass,
                           sta_ssid, sta_pass, host=None, port=None):
    """
    Send settings to ESP32 device via HTTP API
    """

    # Default hostname/IP
    if not host or host == "":
        host = "192.168.4.1"
    else:
        if host.startswith("http://"):
            host = host[7:]
        elif host.startswith("https://"):
            host = host[8:]

        if host.endswith("/"):
            host = host[:-1]

    # Default port
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
        sta_ssid = sta_ssid or ""
        sta_pass = sta_pass or ""

    payload = {
        "deviceName": device_name,
        "wifiMode": wifi_mode,
        "apSsid": ap_ssid,
        "apPass": ap_pass,
        "staSsid": sta_ssid,
        "staPass": sta_pass
    }

    url = f"http://{host}:{port}/settings"

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
                       help="Wi-Fi mode: 'ap' or 'sta'")
    parser.add_argument("-s", "--ssid", required=True,
                       help="SSID (AP or Station depending on mode)")
    parser.add_argument("-p", "--password", required=True,
                       help="Password (AP or Station depending on mode)")
    parser.add_argument("-stssid", "--sta-ssid", required=False,
                       help="Station SSID (optional for AP mode)")
    parser.add_argument("-stpwd", "--sta-pass", required=False,
                       help="Station Password (optional for AP mode)")
    parser.add_argument("-H", "--host", default="",
                       help="Hostname/IP of ESP32 (default 192.168.4.1)")
    parser.add_argument("--port", type=str, default="", help="ESP32 port (default 80)")

    args = parser.parse_args()

    if args.mode == 'ap':
        ap_ssid = args.ssid
        ap_pass = args.password
        sta_ssid = args.sta_ssid or ""
        sta_pass = args.sta_pass or ""
    else:
        sta_ssid = args.ssid
        sta_pass = args.password
        if args.sta_ssid: sta_ssid = args.sta_ssid
        if args.sta_pass: sta_pass = args.sta_pass
        ap_ssid = "ESP32-OTA"
        ap_pass = "esp32pass"

    success = send_settings_to_esp32(
        device_name=args.device,
        wifi_mode=args.mode,
        ap_ssid=ap_ssid,
        ap_pass=ap_pass,
        sta_ssid=sta_ssid,
        sta_pass=sta_pass,
        host=args.host,    
        port=args.port
    )

    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
