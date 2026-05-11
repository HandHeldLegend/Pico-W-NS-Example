#include "main.h"

#include "pico/flash.h"
#include "hardware/flash.h"
#include "hardware/structs/xip_ctrl.h"

#include "pico/btstack_flash_bank.h"
#define FLASH_START_OFFSET (PICO_FLASH_BANK_STORAGE_OFFSET - FLASH_SECTOR_SIZE)

volatile bool _flash_go = false;
uint8_t *_write_from = NULL;
volatile uint32_t _write_size = 0;
volatile uint32_t _write_offset = 0;

uint32_t _get_sector_offset_read(uint32_t page)
{
    // Calculate offset from the end of flash backwards
    uint32_t target_offset = XIP_BASE + FLASH_START_OFFSET - (page * FLASH_SECTOR_SIZE);
    return target_offset;
}

uint32_t _get_sector_offset_write(uint32_t page)
{
    // Calculate offset from the end of flash backwards
    uint32_t target_offset = FLASH_START_OFFSET - (page * FLASH_SECTOR_SIZE);
    return target_offset;
}

// Write data to flash. Different pages can be used to switch between memory items
// Memory being written must match the device sector size
bool ns_flash_write(uint8_t *data, uint32_t size, uint32_t page) 
{
    if(_flash_go) return false;
    if(size > FLASH_SECTOR_SIZE) return false;

    _write_from = data;
    _write_size = size;
    _write_offset = _get_sector_offset_write(page);
    _flash_go = true;

    return true;
}

void _flash_safe_write(void * params)
{
    // Create blank page data
    uint8_t thisPage[FLASH_SECTOR_SIZE] = {0x00};
    // Copy settings into our page buffer
    memcpy(thisPage, _write_from, _write_size);
    // Erase the settings flash sector
    flash_range_erase(_write_offset, FLASH_SECTOR_SIZE);
    // Program the flash sector with our page
    flash_range_program(_write_offset, thisPage, FLASH_SECTOR_SIZE);
}

bool ns_flash_read(uint8_t *out, uint32_t size, uint32_t page) 
{
    if(size > FLASH_SECTOR_SIZE) return false;

    uint32_t offset = _get_sector_offset_read(page);
    const uint8_t *flash_target_contents = (const uint8_t *) (offset);
    memcpy(out, flash_target_contents, size);
    return true;
}

// Should be called from both cores really
void ns_flash_init()
{
    uint core = get_core_num();
    if(core==0)
    {
        flash_safe_execute_core_init();
    }
}

// Run this to apply our flash if
// our flash flag is set
void ns_flash_task()
{
    if(_flash_go)
    {
        flash_safe_execute(_flash_safe_write, NULL, UINT32_MAX);

        _write_from = NULL;
        _write_size = 0;
        _write_offset = 0;

        _flash_go = false;
    }
}
