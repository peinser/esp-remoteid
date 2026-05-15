ESPPORT ?= /dev/ttyESP32
HOST_SERIAL ?= /dev/cu.usbserial-XXXX
SOCAT_PORT ?= 54321
BAUD ?= 115200

.PHONY: help set-target reset menuconfig build flash flash-idf encrypted-flash nvs-flash monitor monitor-raw probe clean bridge-host bridge-container host-setup-socat

help: ## Show available targets
	@awk 'BEGIN {FS = ":.*##"} /^[a-zA-Z0-9_-]+:.*##/ {printf "%-22s %s\n", $$1, $$2}' $(MAKEFILE_LIST)

set-target: ## Configure ESP-IDF target for ESP32-S3
	idf.py set-target esp32s3

reset: ## Reset sdkconfig from sdkconfig.defaults for ESP32-S3
	rm -f sdkconfig sdkconfig.old
	idf.py set-target esp32s3

menuconfig: ## Open ESP-IDF menuconfig
	idf.py menuconfig

build: ## Build firmware
	idf.py build

flash: build ## Flash firmware over the socat bridge (plaintext — first flash or no encryption)
	cd build && esptool.py --chip esp32s3 -p $(ESPPORT) -b 460800 --before no_reset --after no_reset write_flash @flash_args

flash-idf: ## Flash firmware via ESP-IDF reset control (plaintext — first flash or no encryption)
	idf.py -p $(ESPPORT) flash

encrypted-flash: build ## Flash firmware after flash encryption is active (Development mode)
	idf.py -p $(ESPPORT) encrypted-flash

nvs-flash: ## Write nvs.bin to the NVS partition (run before first boot to provision keys)
	parttool.py -p $(ESPPORT) write_partition --partition-name nvs --input nvs.bin

monitor: ## Open ESP-IDF serial monitor via ESPPORT
	idf.py -p $(ESPPORT) monitor

monitor-raw: ## Open a raw serial monitor via ESPPORT without reset control
	python -m serial.tools.miniterm $(ESPPORT) $(BAUD)

probe: ## Probe ESP32-S3 through ESPPORT with esptool
	esptool.py --chip auto -p $(ESPPORT) flash_id

clean: ## Remove ESP-IDF build output
	idf.py fullclean

host-setup-socat: ## Print macOS host socat setup instructions
	@echo "Install socat on macOS if needed: brew install socat"
	@echo "Find the ESP32 serial device: ls /dev/{cu,tty}.usb*"
	@echo "Start host bridge: make bridge-host HOST_SERIAL=/dev/cu.usbmodem21301"
	@echo "Then, inside the devcontainer: make bridge-container"

bridge-host: ## Run on macOS host to expose ESP32-S3 serial over TCP
	socat TCP-LISTEN:$(SOCAT_PORT),reuseaddr,fork FILE:$(HOST_SERIAL),raw,echo=0,ispeed=$(BAUD),ospeed=$(BAUD)

bridge-container: ## Run in devcontainer to create /dev/ttyESP32 from host TCP bridge
	while true; do \
		rm -f $(ESPPORT); \
		socat PTY,link=$(ESPPORT),raw,echo=0,b$(BAUD),mode=0666 TCP:host.docker.internal:$(SOCAT_PORT); \
		printf "ESP serial bridge disconnected; reconnecting in 1s...\n"; \
		sleep 1; \
	done
