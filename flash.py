#!/usr/bin/env python3

import argparse
import requests
import sys
import os

def upload_flash_data_to_esp32(file_path, ip=None, port=None):
    """
    Upload flashdata.bin to ESP32 device via HTTP API and trigger flash

    Args:
        file_path (str): Path to the flashdata.bin file to upload
        ip (str): ESP32 IP address or hostname (default: 192.168.4.1)
        port (int): ESP32 port (default: 80)
    """
    # Use default values if ip or port are None, empty string, or 0
    if not ip or ip == "":
        ip = "192.168.4.1"
    else:
        # Clean up the IP/hostname if it includes protocol or path
        # Remove http:// or https:// prefix if present
        if ip.startswith("http://"):
            ip = ip[7:]  # Remove "http://"
        elif ip.startswith("https://"):
            ip = ip[8:]  # Remove "https://"

        # Remove trailing slash if present
        if ip.endswith("/"):
            ip = ip[:-1]

    if not port or port == "" or port is None:
        port = 80
    else:
        # Convert port to integer if it's provided as string
        try:
            port = int(port)
        except ValueError:
            print(f"Warning: Port '{port}' is not a valid integer. Using default: 80")
            port = 80

    # Check if file exists
    if not os.path.exists(file_path):
        print(f"Error: File '{file_path}' does not exist.")
        return False

    # Construct the upload URL
    upload_url = f"http://{ip}:{port}/upload_bin"

    print(f"Uploading {file_path} to {upload_url}")

    try:
        # Read the binary file and send it as form data
        with open(file_path, 'rb') as file:
            files = {'file': (os.path.basename(file_path), file, 'application/octet-stream')}

            # Send the POST request with the file
            response = requests.post(
                upload_url,
                files=files,
                timeout=60  # Increase timeout for file uploads
            )

            # Log the response as per the web interface
            print(f"[UPLOAD] Response: {response.status_code} {response.text}")

            # Check if the request was successful
            if response.status_code == 200:
                print("File uploaded successfully!")

                # Trigger the flash operation after successful upload
                flash_url = f"http://{ip}:{port}/flash"
                print(f"Triggering flash operation at {flash_url}")

                try:
                    # Make a POST request to the /flash endpoint, similar to the web interface
                    flash_response = requests.post(flash_url, timeout=10)
                    print(f"[FLASH] Response: {flash_response.status_code} {flash_response.text}")

                    # Print flash response as mentioned in the requirement
                    print(f"Flash response: {flash_response.text}")

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
    parser.add_argument("-f", "--file", required=True, help="Path to the flashdata.bin file to upload")
    parser.add_argument("-i", "-ip", "--ip", default="", help="ESP32 IP address or hostname (default: 192.168.4.1 if empty)")
    parser.add_argument("--port", type=str, default="", help="ESP32 port (default: 80 if empty)")

    args = parser.parse_args()

    success = upload_flash_data_to_esp32(
        file_path=args.file,
        ip=args.ip,
        port=args.port
    )

    sys.exit(0 if success else 1)

if __name__ == "__main__":
    main()