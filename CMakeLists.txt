# Copyright (c) 2026 Shaedil
# SPDX-License-Identifier: MIT

if(CONFIG_BOARD_FRAMEWORK_WIRELESS_TP_KB_NRF54LM20A_CPUAPP_ZMK)
  if(CONFIG_USB_DEVICE_STACK)
    message(FATAL_ERROR
      "framework-cb: the legacy USB stack has no driver for this board. Apply "
      "patches/zmk/ and use CONFIG_ZMK_USB_DEVICE_STACK_NEXT, or set "
      "CONFIG_ZMK_USB=n.")
  endif()

  target_sources(app PRIVATE src/status_led.c)
endif()

if(CONFIG_ZMK_USB_DEVICE_STACK_NEXT)
  target_sources(app PRIVATE src/usb_next.c)
endif()

if(CONFIG_FRAMEWORK_CB_WATCHDOG)
  target_sources(app PRIVATE src/watchdog.c)
endif()

if(CONFIG_FRAMEWORK_CB_MODE_SWITCH)
  target_sources(app PRIVATE src/mode_switch.c)
endif()

if(CONFIG_FRAMEWORK_CB_FUEL_GAUGE)
  set(nfg ${CMAKE_CURRENT_LIST_DIR}/third_party/nrf_fuel_gauge)
  if(CONFIG_FP_HARDABI)
    set(float_abi hard-float)
  else()
    set(float_abi soft-float)
  endif()

  zephyr_library_named(framework_cb_fuel_gauge)
  zephyr_library_sources(src/fuel_gauge.c)
  zephyr_library_include_directories(${nfg} ${nfg}/include)
  zephyr_library_link_libraries(${nfg}/lib/cortex-m33/${float_abi}/libnrf_fuel_gauge.a)
endif()
