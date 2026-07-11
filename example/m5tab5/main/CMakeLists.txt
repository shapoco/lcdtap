idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        lib  # the core lcdtap library component; IDF names it after its
             # directory (../../lib), not the lcdtap library.json name
        m5unified
        m5gfx
        driver
        esp_driver_gpio
        esp_driver_parlio
        esp_driver_usb_serial_jtag
        esp_hw_support  # esp_async_memcpy.h, PERIPH_RCC_ATOMIC
        esp_mm
        esp_timer
        nvs_flash
        hal
)
