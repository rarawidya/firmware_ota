# WIFI AP MODE 
| Parameter       | Value          |
| --------------- | -------------- |
| **SSID**        | `STM32-OTA`    |
| **Password**    | `stm32pass`    |
| **Device Name** | `stm32flasher` |


# AT Commands
| AT Command      | Fungtion                               |
| --------------- | -------------------------------------- |
| `AT+DEV_NAME`   | Set/Get device name                    |
| `AT+WIFI_MODE`  | Mode WiFi: AP / STA                    |
| `AT+AP_SSID`    | SSID AP mode                           |
| `AT+AP_PASSWD`  | Password AP mode                       |
| `AT+STA_SSID`   | SSID Station                           |
| `AT+STA_PASSWD` | Password Station                       |
| `AT+FLASH`      | Start flashing process                 |
| `AT+SHOW`       | Show current configuration             |


# Script Python
AP → STA
python3 ./settings.py -d STM32Flasher -m sta -s SSID_WIFI -p PASSWORD_WIFI

AP → AP 
python3 ./settings.py -d STM32Flasher -m ap -s ESP32-OTA -p esp32pass -ip stm32flasher.local

STA → AP
python3 ./settings.py -d STM32Flasher -m ap -s ESP32-OTA -p esp32pass  -ip stm32flasher.local

Flash Firmware
python3 ./flash.py -f ./firmware.bin  -ip stm32flasher.local 