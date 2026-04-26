Import("env")


def erase_nvs_partition(source, target, env):
    import esptool
    import os

    # Get flash settings from platformio environment
    board_config = env.BoardConfig()
    flash_size = board_config.get("upload.flash_size", "4MB")
    flash_freq = board_config.get("upload.flash_freq", "80m")
    flash_mode = board_config.get("upload.flash_mode", "dio")

    # Parse flash size
    if "16MB" in flash_size:
        flash_size_int = 16 * 1024 * 1024
    elif "8MB" in flash_size:
        flash_size_int = 8 * 1024 * 1024
    elif "4MB" in flash_size:
        flash_size_int = 4 * 1024 * 1024
    else:
        flash_size_int = 4 * 1024 * 1024

    # NVS partition is typically at offset 0x3F00000 for 16MB flash (last 1MB)
    # For ESP32-S3, NVS offset is 0x9000 for 4MB, scaled for larger flashes
    nvs_offset = 0x9000  # Default for 4MB, adjust if needed

    if flash_size_int >= 16 * 1024 * 1024:
        nvs_offset = 0x3F00000  # Last 1MB of 16MB flash
    elif flash_size_int >= 8 * 1024 * 1024:
        nvs_offset = 0x1F00000  # Last 1MB of 8MB flash

    print(f"[ERASE NVS] Erasing NVS at offset 0x{nvs_offset:08X}")

    # Get serial port
    from platformio.runner import get_active_board

    board = get_active_board()
    upload_port = env.get("UPLOAD_PORT", "")

    if not upload_port:
        # Try to auto-detect
        import serial.tools.list_ports

        ports = list(serial.tools.list_ports.comports())
        for port in ports:
            if "USB" in port.device or "ACM" in port.device or "tty" in port.device:
                upload_port = port.device
                break

    if not upload_port:
        print("[ERASE NVS] No serial port found, skipping")
        return

    try:
        esptool.main(
            [
                "--chip",
                "esp32s3",
                "--port",
                upload_port,
                "--baud",
                "460800",
                "erase_region",
                f"0x{nvs_offset:08X}",
                "0x10000",  # Erase 64KB for NVS
            ]
        )
        print("[ERASE NVS] NVS erased successfully")
    except Exception as e:
        print(f"[ERASE NVS] Warning: {e}")


env.AddPostAction("upload", erase_nvs_partition)
