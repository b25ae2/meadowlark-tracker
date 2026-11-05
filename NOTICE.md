# Third-Party Notices & Attributions

Polyend Tracker Community Edition (CE) incorporates or links against the following third-party open-source and binary components:

---

## 1. ARM CMSIS & CMSIS-DSP
* **Location**: `platforms/imx/CMSIS/`, `platforms/mk66/3rdparty/teensy_core/`
* **License**: Apache License 2.0 / BSD-3-Clause
* **Copyright**: Copyright (c) ARM Limited. All rights reserved.

## 2. NXP MCUXpresso SDK (Kinetis K66 & i.MX RT1050/RT1060)
* **Location**: `platforms/imx/drivers/`, `platforms/imx/device/`, `platforms/imx/usb/`, `platforms/imx/sdmmc/`, `platforms/mk66/3rdparty/SD/`, `platforms/mk66/3rdparty/I2C/`, `platforms/mk66/3rdparty/SDRAM/`
* **License**: BSD-3-Clause
* **Copyright**: Copyright (c) Freescale Semiconductor, Inc. / Copyright 2016-2022 NXP.

## 3. FatFs - Generic FAT Filesystem Module
* **Location**: `platforms/imx/fatfs/`, `platforms/mk66/3rdparty/SD/fatfs/`
* **License**: FatFs License (BSD-style)
* **Copyright**: Copyright (C) ChaN, 2019. All rights reserved.

## 4. Teensy Cores & Teensy Audio Library (PJRC)
* **Location**: `platforms/mk66/3rdparty/teensy_core/`, `platforms/mk66/3rdparty/Audio/`, `core/libs/Audio/`
* **License**: MIT / PJRC Permissive License
* **Copyright**: Copyright (c) 2014-2018 Paul Stoffregen, PJRC.COM, LLC.

## 5. Liberation Sans Fonts (UI Bitmap Font Source)
* **Location**: `core/libs/fonts/LiberationSans-*_L4.{h,cpp}` (4-bit `L4` antialiased glyph bitmaps)
* **License**: SIL Open Font License 1.1 (OFL-1.1)
* **Copyright**: Digitized data copyright (c) 2010 Google Corporation; Copyright (c) 2012 Red Hat, Inc.

## 6. Embedded Template Library (ETL)
* **Location**: `util/3rdparty/etl/`
* **License**: MIT License
* **Copyright**: Copyright (c) 2016 jwellbelove (John Wellbelove).

## 7. magic_enum
* **Location**: `util/3rdparty/magic_enum/`
* **License**: MIT License
* **Copyright**: Copyright (c) 2019 - 2022 Daniil Goncharov.

## 8. FastCRC
* **Location**: `platforms/mk66/3rdparty/FastCRC/`, `platforms/imx/3rdparty/FastCRC/`
* **License**: MIT / GPLv3 dual-licensed (used under MIT/BSD terms where applicable)
* **Copyright**: Copyright (c) 2014-2016 Frank Boesing.

## 9. Polyend Precompiled Synth & DSP Libraries (`libPolySynth.a`, `libPolyDSP.a`)
* **Location**: `synth/lib/`
* **License**: Polyend Precompiled Synthesizer & DSP Library License (`synth/lib/LICENSE-PROPRIETARY.txt`)
* **Copyright**: Copyright (c) 2023-2026 Polyend Sp. z o.o. All rights reserved.

## 10. Desktop build (Meadowlark, `platforms/desktop/`)
Compiled into the desktop app:
* **stb_image** (`util/3rdparty/stb/`): public domain / MIT, Sean Barrett.
* **CLAP headers** (`util/3rdparty/clap/`): MIT License, Copyright (c) 2021 Alexandre Bique.
* **toml++** (system headers, used header-only): MIT License, Copyright (c) Mark Gillard.
* **LV2 headers**: ISC License, Copyright (c) 2006-2020 Steve Harris, David Robillard and others.

Linked dynamically (on macOS, copied into `Meadowlark.app`):
* **SDL 3**: zlib License, Copyright (c) 1997-2025 Sam Lantinga.
* **FreeType**: FreeType License (FTL). Portions of this software are copyright (c) The
  FreeType Project (www.freetype.org). All rights reserved.
* **lilv, serd, sord, sratom, zix**: ISC License, Copyright (c) 2007-2024 David Robillard.
* **libpng** (FreeType's, macOS app): PNG Reference Library License version 2, Copyright (c)
  1995-2024 The PNG Reference Library Authors.
* Linux only, from the system: **PipeWire** (MIT), **ALSA library** (LGPL-2.1), **Xlib**
  (MIT/X11).
