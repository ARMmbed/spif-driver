/* mbed Microcontroller Library
 * Copyright (c) 2016 ARM Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "SPIFBlockDevice.h"
#include "mbed_critical.h"

// Read/write/erase sizes
#define SPIF_READ_SIZE  1
#define SPIF_PROG_SIZE  1
#define SPIF_SE_SIZE    4096
#define SPIF_TIMEOUT    10000

// Debug available
#define SPIF_DEBUG      0

// MX25R Series Register Command Table. 
enum ops {
    SPIF_NOP  = 0x00, // No operation
    SPIF_READ = 0x03, // Read data
    SPIF_PROG = 0x02, // Program data
    SPIF_SE   = 0x20, // 4KB Sector Erase 
    SPIF_CE   = 0xc7, // Chip Erase 
    SPIF_SFDP = 0x5a, // Read SFDP 
    SPIF_WREN = 0x06, // Write Enable 
    SPIF_WRDI = 0x04, // Write Disable
    SPIF_RDSR = 0x05, // Read Status Register 
    SPIF_RDID = 0x9f, // Read Manufacturer and JDEC Device ID 
};

// Status register from RDSR
// [- stuff -| wel | wip ]
// [-   6   -|  1  |  1  ]
#define SPIF_WEL 0x2
#define SPIF_WIP 0x1

 
SPIFBlockDevice::SPIFBlockDevice(
    PinName mosi, PinName miso, PinName sclk, PinName cs, int freq)
    : _spi(mosi, miso, sclk), _cs(cs), _size(0), _is_initialized(false), _init_ref_count(0)
{
    _cs = 1;
    _spi.frequency(freq);
}

int SPIFBlockDevice::init()
{
    if (!_is_initialized) {
        _init_ref_count = 0;
    }

    uint32_t val = core_util_atomic_incr_u32(&_init_ref_count, 1);

    if (val != 1) {
        return BD_ERROR_OK;
    }

    // Check for vendor specific hacks, these should move into more general
    // handling when possible. RDID is not used to verify a device is attached.
    uint8_t id[3];
    _cmdread(SPIF_RDID, 0, 3, 0x0, id);

    switch (id[0]) {
        case 0xbf:
            // SST devices come preset with block protection
            // enabled for some regions, issue gbpu instruction to clear
            _wren();
            _cmdwrite(0x98, 0, 0, 0x0, NULL);
            break;
    }

    // Check that device is doing ok 
    int err = _sync();
    if (err) {
        return BD_ERROR_DEVICE_ERROR;
    }

    // Check JEDEC serial flash discoverable parameters for device
    // specific info
    uint8_t header[16];
    _cmdread(SPIF_SFDP, 4, 16, 0x0, header);

    // Verify SFDP signature for sanity
    // Also check that major/minor version is acceptable
    if (!(memcmp(&header[0], "SFDP", 4) == 0 && header[5] == 1)) {
        return BD_ERROR_DEVICE_ERROR;
    }

    // The SFDP spec indicates the standard table is always at offset 0
    // in the parameter headers, we check just to be safe
    if (!(header[8] == 0 && header[10] == 1)) {
        return BD_ERROR_DEVICE_ERROR;
    }

    // Parameter table pointer, spi commands are BE, SFDP is LE,
    // also sfdp command expects extra read wait byte
    uint32_t table_addr = (
        (header[14] << 24) |
        (header[13] << 16) |
        (header[12] << 8 ));
    uint8_t table[8];
    _cmdread(SPIF_SFDP, 4, 8, table_addr, table);

    // Check erase size, currently only supports 4kbytes
    // TODO support erase size != 4kbytes?
    // TODO support other erase opcodes from the sector descriptions
    if ((table[0] & 0x3) != 0x1 || table[1] != SPIF_SE) {
        return BD_ERROR_DEVICE_ERROR;
    }

    // Check address size, currently only supports 3byte addresses
    // TODO support address > 3bytes?
    // TODO check for devices larger than 2Gbits?
    if ((table[2] & 0x4) != 0 || (table[7] & 0x80) != 0) {
        return BD_ERROR_DEVICE_ERROR;
    }

    // Get device density, stored as size in bits - 1
    uint32_t density = (
        (table[7] << 24) |
        (table[6] << 16) |
        (table[5] << 8 ) |
        (table[4] << 0 ));
    _size = (density/8) + 1;

    _is_initialized = true;
    return 0;
}

int SPIFBlockDevice::deinit()
{
    if (!_is_initialized) {
        _init_ref_count = 0;
        return 0;
    }

    uint32_t val = core_util_atomic_decr_u32(&_init_ref_count, 1);

    if (val) {
        return 0;
    }

    // Latch write disable just to keep noise
    // from changing the device
    _cmdwrite(SPIF_WRDI, 0, 0, 0x0, NULL);

    _is_initialized = false;
    return 0;
}

void SPIFBlockDevice::_cmdread(
        uint8_t op, uint32_t addrc, uint32_t retc,
        uint32_t addr, uint8_t *rets)
{
    _cs = 0;
    _spi.write(op);

    for (uint32_t i = 0; i < addrc; i++) {
        _spi.write(0xff & (addr >> (8 * (addrc - 1 - i))));
    }

    for (uint32_t i = 0; i < retc; i++) {
        rets[i] = _spi.write(0);
    }
    _cs = 1;

    if (SPIF_DEBUG) {
        printf("spif <- %02x", op);
        for (uint32_t i = 0; i < addrc; i++) {
            if (i < addrc) {
                printf("%02lx", 0xff & (addr >> (8 * (addrc - 1 - i))));
            } else {
                printf("  ");
            }
        }
        printf(" ");
        for (uint32_t i = 0; i < 16 && i < retc; i++) {
            printf("%02x", rets[i]);
        }
        if (retc > 16) {
            printf("...");
        }
        printf("\n");
    }
}

void SPIFBlockDevice::_cmdwrite(
        uint8_t op, uint32_t addrc, uint32_t argc,
        uint32_t addr, const uint8_t *args)
{
    _cs = 0;
    _spi.write(op);

    for (uint32_t i = 0; i < addrc; i++) {
        _spi.write(0xff & (addr >> (8 * (addrc - 1 - i))));
    }

    for (uint32_t i = 0; i < argc; i++) {
        _spi.write(args[i]);
    }
    _cs = 1;

    if (SPIF_DEBUG) {
        printf("spif -> %02x", op);
        for (uint32_t i = 0; i < addrc; i++) {
            if (i < addrc) {
                printf("%02lx", 0xff & (addr >> (8 * (addrc - 1 - i))));
            } else {
                printf("  ");
            }
        }
        printf(" ");
        for (uint32_t i = 0; i < 16 && i < argc; i++) {
            printf("%02x", args[i]);
        }
        if (argc > 16) {
            printf("...");
        }
        printf("\n");
    }
}

int SPIFBlockDevice::_sync()
{
    for (int i = 0; i < SPIF_TIMEOUT; i++) {
        // Read status register until write not-in-progress
        uint8_t status;
        _cmdread(SPIF_RDSR, 0, 1, 0x0, &status);

        // Check WIP bit
        if (!(status & SPIF_WIP)) {
            return 0;
        }

        wait_ms(1);
    }

    return BD_ERROR_DEVICE_ERROR;
}

int SPIFBlockDevice::_wren()
{
    _cmdwrite(SPIF_WREN, 0, 0, 0x0, NULL);

    for (int i = 0; i < SPIF_TIMEOUT; i++) {
        // Read status register until write latch is enabled
        uint8_t status;
        _cmdread(SPIF_RDSR, 0, 1, 0x0, &status);

        // Check WEL bit
        if (status & SPIF_WEL) {
            return 0;
        }

        wait_ms(1);
    }

    return BD_ERROR_DEVICE_ERROR;
}

int SPIFBlockDevice::read(void *buffer, bd_addr_t addr, bd_size_t size)
{
    if (!_is_initialized) {
        return BD_ERROR_DEVICE_ERROR;
    }

    // Check the address and size fit onto the chip.
    MBED_ASSERT(is_valid_read(addr, size));

    _cmdread(SPIF_READ, 3, size, addr, static_cast<uint8_t *>(buffer));
    return 0;
}
 
int SPIFBlockDevice::program(const void *buffer, bd_addr_t addr, bd_size_t size)
{
    // Check the address and size fit onto the chip.
    MBED_ASSERT(is_valid_program(addr, size));

    if (!_is_initialized) {
        return BD_ERROR_DEVICE_ERROR;
    }

    while (size > 0) {
        int err = _wren();
        if (err) {
            return err;
        }

        // Write up to 256 bytes a page
        // TODO handle unaligned programs
        uint32_t off = addr % 256;
        uint32_t chunk = (off + size < 256) ? size : (256-off);
        _cmdwrite(SPIF_PROG, 3, chunk, addr, static_cast<const uint8_t *>(buffer));
        buffer = static_cast<const uint8_t*>(buffer) + chunk;
        addr += chunk;
        size -= chunk;

        wait_ms(1);

        err = _sync();
        if (err) {
            return err;
        }
    }

    return 0;
}

int SPIFBlockDevice::erase(bd_addr_t addr, bd_size_t size)
{
    // Check the address and size fit onto the chip.
    MBED_ASSERT(is_valid_erase(addr, size));

    if (!_is_initialized) {
        return BD_ERROR_DEVICE_ERROR;
    }

    while (size > 0) {
        int err = _wren();
        if (err) {
            return err;
        }
    
        // Erase 4kbyte sectors
        // TODO support other erase sizes?
        uint32_t chunk = 4096;
        _cmdwrite(SPIF_SE, 3, 0, addr, NULL);
        addr += chunk;
        size -= chunk;

        err = _sync();
        if (err) {
            return err;
        }
    }

    return 0;
}

bd_size_t SPIFBlockDevice::get_read_size() const
{
    return SPIF_READ_SIZE;
}

bd_size_t SPIFBlockDevice::get_program_size() const
{
    return SPIF_PROG_SIZE;
}

bd_size_t SPIFBlockDevice::get_erase_size() const
{
    return SPIF_SE_SIZE;
}

bd_size_t SPIFBlockDevice::get_erase_size(bd_addr_t addr) const
{
    return SPIF_SE_SIZE;
}

bd_size_t SPIFBlockDevice::size() const
{
    if (!_is_initialized) {
        return BD_ERROR_DEVICE_ERROR;
    }

    return _size;
}

int SPIFBlockDevice::get_erase_value() const
{
    return 0xFF;
}
