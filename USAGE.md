ap to sta: python3 ./settings.py -d STM32Flasher -m sta -s RR -p 12345678
ap to ap: python3 ./settings.py -d STM32Flasher -m ap -s ESP32-OTA -p esp32pass -ip stm32flasher.local
sta to ap: python3 ./settings.py -d STM32Flasher -m ap -s ESP32-OTA -p esp32pass  -ip stm32flasher.local

flash : 
python3 ./flash.py -f ./blink500.bin  -ip stm32flasher.local 