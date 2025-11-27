#!/usr/bin/env python3

import argparse
import requests
import sys
import os

def upload_flash_data_to_esp32(file_path, host=None, port=None):
    """
    Upload flashdata.bin to ESP32 device via HTTP API and trigger flash

    Args:
        file_path (str): Path to the flashdata.bin file to upload
        host (str): ESP32 IP address or hostname (default: 192.168.4.1)
        port (int): ESP32 port (default: 80)
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

    if not os.path.exists(file_path):
        print(f"Error: File '{file_path}' does not exist.")
        return False

    upload_url = f"http://{host}:{port}/upload_bin"

    print(f"Uploading {file_path} to {upload_url}")

    try:
        with open(file_path, 'rb') as file:
            files = {'file': (os.path.basename(file_path), file, 'application/octet-stream')}

            response = requests.post(
                upload_url,
                files=files,
                timeout=60  
            )

            print(f"[UPLOAD] Response: {response.status_code} {response.text}")

            if response.status_code == 200:
                print("File uploaded successfully!")

                flash_url = f"http://{host}:{port}/flash"
                print(f"Triggering flash operation at {flash_url}")

                try:
                    flash_response = requests.post(flash_url, timeout=10)
                    print(f"[FLASH] Response: {flash_response.status_code} {flash_response.text}")

                    if flash_response.status_code == 200:
                        print("Flash operation started successfully!")
                        return True
                    else:
                        print(f"Failed to start flash operation. Status code: {flash_response.status_code}")
                        return False

                except requests.exceptions.ConnectionError:
                    print(f"Error: Could not connect to {flash_url}. Please check if the ESP32 is accessible.")
                    return False
                except requests.exceptions.Timeout:
                    print(f"Error: Request timed out while connecting to {flash_url}.")
                    return False
                except Exception as e:
                    print(f"Unexpected error during flash operation: {str(e)}")
                    return False

            else:
                print(f"Failed to upload file. Status code: {response.status_code}")
                return False

    except requests.exceptions.ConnectionError:
        print(f"Error: Could not connect to {upload_url}. Please check if the ESP32 is accessible.")
        return False
    except requests.exceptions.Timeout:
        print(f"Error: Request timed out while connecting to {upload_url}. Upload may be taking longer than expected.")
        return False
    except Exception as e:
        print(f"Unexpected error: {str(e)}")
        return False


def main():
    parser = argparse.ArgumentParser(description="Upload flashdata.bin to ESP32 OTA device")
    parser.add_argument("-f", "--file", required=True, help="Path to flashdata.bin file")
    parser.add_argument("-H", "--host", default="", help="ESP32 hostname/IP (default: 192.168.4.1)")

    parser.add_argument("--port", type=str, default="", help="ESP32 port (default: 80)")

    args = parser.parse_args()

    success = upload_flash_data_to_esp32(
        file_path=args.file,
        host=args.host,
        port=args.port
    )

    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
