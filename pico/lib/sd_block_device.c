#define __GNU_VISIBLE 1
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "pico/multicore.h"
#include "receive_fifo.pio.h"
#include "transmit_fifo.pio.h"
#include "../sdio-fatfs/include/FatFsSd_C.h"
#include "../sdio-fatfs/src/include/f_util.h"
#include "../sdio-fatfs/src/ff15/source/ff.h"

#include "../../common/protocols.h"
#include "../../common/dos_device_payloads.h"
#include "../../common/crc8.h"
#include "pico_common.h"
#include "sd_block_device.h"

/* SDIO Interface */
static sd_sdio_if_t sdio_if = {
    /*
    Pins CLK_gpio, D1_gpio, D2_gpio, and D3_gpio are at offsets from pin D0_gpio.
    The offsets are determined by sd_driver\SDIO\rp2040_sdio.pio.
        CLK_gpio = (D0_gpio + SDIO_CLK_PIN_D0_OFFSET) % 32;
        As of this writing, SDIO_CLK_PIN_D0_OFFSET is 30,
            which is -2 in mod32 arithmetic, so:
        CLK_gpio = D0_gpio -2.
        D1_gpio = D0_gpio + 1;
        D2_gpio = D0_gpio + 2;
        D3_gpio = D0_gpio + 3;
    */
    .CMD_gpio = 1,
    .D0_gpio = 2,
    .baud_rate = 15 * 1000 * 1000  // 15 MHz
};

/* Hardware Configuration of the SD Card socket "object" */
static sd_card_t sd_card_sdio = {
    .type = SD_IF_SDIO,
    .sdio_if_p = &sdio_if
};

/* SPI Interface */
 static spi_t spi = {  
     .hw_inst = spi0,  // RP2040 SPI component
     .sck_gpio = 2,    // GPIO number (not Pico pin number)
     .mosi_gpio = 3,
     .miso_gpio = 4,
     .baud_rate = 12 * 1000 * 1000    // Actual frequency: 10416666.
 };

 /* SPI Interface */
 static sd_spi_if_t spi_if = {
     .spi = &spi,  // Pointer to the SPI driving this card
     .ss_gpio = 5       // The SPI slave select GPIO for this SD card
 };

/* Configuration of the SD Card socket object */
 static sd_card_t sd_card = {   
     .type = SD_IF_SPI,
     .spi_if_p = &spi_if   // Pointer to the SPI interface driving this card
 };


/* Callbacks used by the library: */
size_t sd_get_num() { 
    return 1; 
}

sd_card_t *sd_get_by_num(size_t num) {
    if (0 == num)
        return &sd_card;
    else
        return NULL;
}


// Function to check if a file matches the given pattern
int matches_pattern(const char *filename) {
    if (strlen(filename) < 8) return 0; // Minimum length for valid filenames (e.g., 0_pc.img)

    // Check for "X_pc" or "X_v9k" pattern where X is a digit
    if ((strcasestr(filename, "_pc") != 0 || strcasestr(filename, "_v9k") != 0) &&
        strcasestr(filename, ".img") != 0 && filename[0] != '.') {
        return 1;
    }
    return 0;
}


// Function to read a sector from the image file
int read_sector(FIL *img_file, uint32_t sector_number, uint8_t *buffer) {
    FRESULT res;
    UINT bytes_read;

    // Move the file pointer to the desired sector
    FSIZE_t offset = (FSIZE_t)sector_number * SECTOR_SIZE;
    res = f_lseek(img_file, offset);
    if (res != FR_OK) {
        printf("f_lseek failed: %d\n", res);
        return -1;
    }

    // Read SECTOR_SIZE bytes into the buffer
    res = f_read(img_file, buffer, SECTOR_SIZE, &bytes_read);
    if (res != FR_OK) {
        printf("f_read failed: %d\n", res);
        return -1;
    }
    if (bytes_read != SECTOR_SIZE) {
        printf("Incomplete read: expected %u bytes, got %u bytes\n", SECTOR_SIZE, bytes_read);
        return -1;
    }

    return 0;
}

// Function to parse the partition table and get the starting sector of the first partition
uint32_t get_first_partition_start(uint8_t *mbr_buffer) {
    // Partition table starts at byte offset 446
    uint8_t *partition_entry = mbr_buffer + 446;
    // Starting sector is a 4-byte little-endian value at offset 8 within the partition entry
    uint32_t start_sector = partition_entry[8] |
                            (partition_entry[9] << 8) |
                            (partition_entry[10] << 16) |
                            (partition_entry[11] << 24);
    return start_sector;
}

/* Function to parse the BPB from a FAT16 .img file */
int parse_fat16_bpb(FIL *img_file, VictorBPB *bpb) {

    FRESULT res;
    uint8_t buffer[SECTOR_SIZE];

    uint8_t boot_sector[SECTOR_SIZE]; 
    size_t bytes_read;

    // Read MBR from sector 0
    if (read_sector(img_file, 0, buffer) != 0) {
        f_close(img_file);
        return -1;
    }

     // Get the starting sector of the first partition
    uint32_t partition_start = get_first_partition_start(buffer);
    printf("First partition starts at sector: %u\n", partition_start);

    // Read the boot sector of the first partition
    if (read_sector(img_file, partition_start, boot_sector) != 0) {
        f_close(img_file);
        return -1;
    }


    /* Parse the BPB fields from the boot sector */
    bpb->bytes_per_sector     = boot_sector[11] | (boot_sector[12] << 8);
    bpb->sectors_per_cluster  = boot_sector[13];
    bpb->reserved_sectors     = boot_sector[14] | (boot_sector[15] << 8);
    bpb->num_fats             = boot_sector[16];
    bpb->root_entry_count     = boot_sector[17] | (boot_sector[18] << 8);
    bpb->total_sectors        = boot_sector[19] | (boot_sector[20] << 8);
    bpb->media_descriptor     = boot_sector[21];
    bpb->sectors_per_fat      = boot_sector[22] | (boot_sector[23] << 8);
    bpb->sectors_per_track    = boot_sector[24] | (boot_sector[25] << 8);
    bpb->num_heads            = boot_sector[26] | (boot_sector[27] << 8);
    bpb->hidden_sectors       = boot_sector[28] |
                               (boot_sector[29] << 8) |
                               (boot_sector[30] << 16) |
                               (boot_sector[31] << 24);
    printf("BPB fields parsed successfully\n");
    printf("Bytes per sector: %d\n", bpb->bytes_per_sector);
    printf("Sectors per cluster: %d\n", bpb->sectors_per_cluster);
    printf("Reserved sectors: %d\n", bpb->reserved_sectors);
    printf("Number of FATs: %d\n", bpb->num_fats);
    printf("Root entry count: %d\n", bpb->root_entry_count);
    printf("Total sectors: %d\n", bpb->total_sectors);
    printf("Media descriptor: 0x%02X\n", bpb->media_descriptor);
    printf("Sectors per FAT: %d\n", bpb->sectors_per_fat);
    printf("Sectors per track: %d\n", bpb->sectors_per_track);
    printf("Number of heads: %d\n", bpb->num_heads);
    printf("Hidden sectors: %d\n", bpb->hidden_sectors);

    return 0; /* Success */
}

/* Function to find the Working Media List (WML) */
uint8_t* find_v9k_working_media_list(V9KDiskLabel* dlbl) {
    /* The WML follows immediately after the variable lists */
    uint8_t* var_lists = dlbl->var_data;
    uint8_t var_lists_size = var_lists[0];
    return &var_lists[var_lists_size * 8 + 1];
}

/* Function to find the Virtual Volume List (VVlist) */
uint8_t* find_v9k_virtual_volume_list(V9KDiskLabel* dlbl) {
    uint8_t* var_lists = dlbl->var_data;
    uint8_t var_lists_size = var_lists[0];
    uint8_t* wml = &var_lists[var_lists_size * 8 + 1];
    uint8_t wml_size = wml[0];
    return &wml[wml_size * 8 + 1];
}

int parse_victor9000_disk_label(FIL *img_file, VictorBPB *bpb) {
    
    uint8_t sector0[1024]; /* Read more than one sector if necessary */
    size_t bytes_read;
    V9KDiskLabel *dlbl = NULL;
    uint8_t *var_data;
    uint8_t *wml;
    uint8_t wml_region_count;
    uint8_t *vvlist;
    uint8_t vvlist_volume_count;
    uint32_t total_sectors = 0;
    uint16_t sectors_per_track = 0;
    uint16_t num_heads = 0;
    int i;

    /* Read enough data to cover the disk label and variable-length fields */
    FRESULT fr = f_read(img_file, sector0, sizeof(sector0), &bytes_read);
    if (bytes_read < sizeof(V9KDiskLabel)) {
        perror("Error reading disk label");
        f_close(img_file);
        return -1;
    }

    /* Map sector0 to the DiskLabel structure */
    dlbl = (V9KDiskLabel *)sector0;

    /* Extract Control Parameters */
    V9KHardDriveControlParameters *ctrl_params = &dlbl->ctrl_params;

    /* Get number of heads */
    num_heads = ctrl_params->num_heads;

    /* Get sectors per track */
    /* Note: The interleave field may not represent sectors per track */
    /* You may need to obtain sectors per track from additional information */
    sectors_per_track = 17; /* Placeholder value */

    /* Locate the variable-length data */
    var_data = dlbl->var_data;

    /* Find the Working Media List (WML) */
    uint8_t var_lists_size = var_data[0];
    wml = &var_data[var_lists_size * 8 + 1]; /* Skip over variable lists */

    /* Read the WML */
    wml_region_count = wml[0];
    uint8_t *wml_data = &wml[1];

    /* Each region descriptor is 8 bytes (2 DWORDs) */
    for (i = 0; i < wml_region_count; i++) {
        if ((wml_data + i * 8 + 8) > (sector0 + bytes_read)) {
            fprintf(stderr, "Error: Exceeded buffer size while reading WML\n");
            return -1;
        }
        uint32_t region_pa = *(uint32_t *)(wml_data + i * 8);
        uint32_t region_size = *(uint32_t *)(wml_data + i * 8 + 4);
        total_sectors += region_size;
    }

    /* Populate the BPB structure */
    bpb->bytes_per_sector = dlbl->sector_size;
    bpb->sectors_per_cluster = 4; /* Adjust as needed */
    bpb->reserved_sectors = 1;    /* Typically 1 for the boot sector */
    bpb->num_fats = 2;            /* Common value */
    bpb->root_entry_count = 512;  /* Common value */
    bpb->total_sectors = (uint16_t)total_sectors; /* May need to handle larger values */
    bpb->media_descriptor = 0xF8; /* Standard hard disk media descriptor */
    bpb->sectors_per_fat = 9;     /* Placeholder; calculate based on filesystem */
    bpb->sectors_per_track = sectors_per_track;
    bpb->num_heads = num_heads;
    bpb->hidden_sectors = 0;      /* Assuming partition starts at sector 0 */

    return 0; /* Success */
}

SDState* initialize_sd_state(const char *directory) {
    printf("Initializaing SD Card...\n");
    SDState *sdState = malloc(sizeof(SDState));
    if (!sdState) {
        perror("Failed to allocate SDState");
        return NULL;
    }

    sdState->fs = malloc(sizeof(FATFS));
    if (!sdState->fs) {
        perror("Failed to allocate FATFS");
        free(sdState);
        return NULL;
    }
    FRESULT fr = f_mount(sdState->fs, "", 1);
    if (FR_OK != fr) panic("f_mount error: %s (%d)\n", FRESULT_str(fr), fr);

    const char* const filename = "output.log";
    sdState->debug_log = malloc(sizeof(FIL));
    if (!sdState->debug_log) {
        perror("Failed to allocate FIL for debug_log");
        // Handle cleanup and error
    }
    fr = f_open(sdState->debug_log, filename, FA_OPEN_APPEND | FA_WRITE);
    if (FR_OK != fr && FR_EXIST != fr)
        panic("f_open(%s) error: %s (%d)\n", filename, FRESULT_str(fr), fr);
    if (f_printf(sdState->debug_log, "Hello, world!\n") < 0) {
        printf("fprintf failed\n");
    }

    printf("Mounted SD card\n");
    DIR dir;
    struct dirent *entry;
    sdState->fileCount = 0;

    fr = f_opendir(&dir, directory);
    if (FR_OK != fr) {
        perror("opendir");
        free(sdState);
        return NULL;
    }

    FILINFO fno;
    sdState->fileCount = 0;
    while (FR_OK == f_readdir(&dir, &fno) ) {
        printf("directory entry: %s\n", fno.fname);
        if (fno.fname[0] == 0 || sdState->fileCount >= MAX_IMG_FILES) {
            printf("No more files\n");
            break;
        }
        if (matches_pattern(fno.fname)) {
            printf("Found matching file: %d %s\n", sdState->fileCount, fno.fname);
            strncpy(sdState->file_names[sdState->fileCount], fno.fname, FILENAME_MAX_LENGTH - 1);
            sdState->file_names[sdState->fileCount][FILENAME_MAX_LENGTH - 1] = '\0';
            sdState->img_file[sdState->fileCount] = malloc(sizeof(FIL));
            if (!sdState->img_file[sdState->fileCount]) {
                perror("Failed to allocate FIL");
                // Handle cleanup and error
            }
            FRESULT fr = f_open(sdState->img_file[sdState->fileCount], fno.fname, FA_OPEN_EXISTING | FA_READ | FA_WRITE);
            if (FR_OK != fr) {
                printf("error opening file, %s", fno.fname);
            }
            sdState->fileCount++;
        }
    }
    f_closedir(&dir);
    printf("file list length: %d\n", sdState->fileCount);

    // Loop through and print each string
    for (int i = 0; i < sdState->fileCount; ++i) {
        printf("sdState->file_names[%s]: \n", sdState->file_names[i]);
    }
    return sdState;
}

void freeSDState(SDState *sdState) {
    for (int i = 0; i < sdState->fileCount; i++) {
        f_close(sdState->img_file[i]);
    }
    f_close(sdState->debug_log);
    f_unmount("");
    free(sdState->fs);
    free(sdState);
}


//returns an arraay of BPBs used to initialize the drives
Payload* init_sd_card(SDState *sdState, PIO_state *pio_state, Payload *payload) {

    printf("Found %d matching files:\n", sdState->fileCount);
    for (int i = 0; i < sdState->fileCount; i++) {
        printf("%s\n", sdState->file_names[i]);
    }
       
    //determine the number of drives I'm instantiating, based on images from SD card
    uint8_t num_drives = sdState->fileCount;

    //initialize the response payload
    Payload *response = (Payload*)malloc(sizeof(Payload));
    if (response == NULL) {
        printf("Error: Memory allocation failed for payload\n");
        return response;
    }
    memset(response, 0, sizeof(Payload));
    response->protocol = SD_BLOCK_DEVICE;
    response->command = DEVICE_INIT;
    response->params_size = 1;
    response->params = (uint8_t *)malloc(1);
    if (response->params == NULL) {
        printf("Error: Memory allocation failed for response->params\n");
        return response;
    }
    response->params[0] = 0;

    //InitPayload is a struct that contains the number of drives and an array of BPBs
    //for DOS INIT call. Don't confuse with the protocol Payload which is the wire format
    InitPayload *initPayload = (InitPayload *)malloc(sizeof(InitPayload));
    if (initPayload == NULL) {
        printf("Error: Memory allocation failed for initPayload\n");
        response->status = MEMORY_ALLOCATION_ERROR;
        return response;
    }
    memset(initPayload, 0, sizeof(InitPayload));

    initPayload->num_units = num_drives;
    
    //parse the BPB for each image file
    uint8_t i;
    for (i = 0; i < num_drives; i++) {
        printf("Parsing BPB for %s\n", sdState->file_names[i]);
        if (strcasestr(sdState->file_names[i], "_v9k") != 0) {
            if (parse_victor9000_disk_label(sdState->img_file[i], &initPayload->bpb_array[i]) != 0) {
                printf("Error parsing BPB for %s\n", sdState->file_names[i]);
                return false;
            }
        } else {
            if (parse_fat16_bpb(sdState->img_file[i], &initPayload->bpb_array[i]) != 0) {
                printf("Error parsing BPB for %s\n", sdState->file_names[i]);
                return false;
            }
        }
    }

    //return the drive information to the Victor 9000
    response->data = (uint8_t *)initPayload;
    response->data_size = (sizeof(InitPayload));
    create_command_crc8(response);
    create_data_crc8(response);
   
    return response;

}

Payload* media_check(SDState *sdState, PIO_state *pio_state, Payload *payload) {
    //fully in RAM on Victor 9000, not needed here
}

Payload* build_bpb(SDState *sdState, PIO_state *pio_state, Payload *payload) {
    //fully in RAM on Victor 9000, not needed here
}

Payload* victor9k_drive_info(SDState *sdState, PIO_state *pio_state, Payload *payload) {
    //fully in RAM on Victor 9000, not needed here
}

Payload* sd_read(SDState *sdState, PIO_state *pio_state, Payload *payload) {
    ReadParams *readParams = (ReadParams *)payload->params;

    Payload *response = (Payload*)malloc(sizeof(Payload));
    if (response == NULL) {
        printf("Error: Memory allocation failed for payload\n");
        return NULL;
    }
    memset(response, 0, sizeof(Payload));
    response->protocol = SD_BLOCK_DEVICE;
    response->command = READ_BLOCK;
    response->params = (uint8_t *)malloc(1);
    if (response->params == NULL) {
        printf("Error: Memory allocation failed for response->params\n");
        free(response);
        return NULL;
    }
    response->params[0] = 0;

    int driveNumber = readParams->drive_number;

    // Calculate the offset in the .img file
    int startSector = readParams->start_sector;
    long offset = startSector * SECTOR_SIZE;

    // Move to the calculated offset
    if (FR_OK != f_lseek(sdState->img_file[driveNumber], offset)) {
        printf("Failed to seek to offset");
        response->status = FILE_SEEK_ERROR;
        free(response->params);
        free(response);
        return NULL;
    }

    // Calculate the number of bytes to read
    int sectorCount = readParams->sector_count;
    size_t bytesToRead = sectorCount * SECTOR_SIZE;

    // Read the data into the buffer
    char *buffer = (char *)malloc(bytesToRead);
    if (buffer == NULL) {
        printf("Failed to allocate buffer");
        response->status = MEMORY_ALLOCATION_ERROR;
        free(response->params);
        free(response);
        return NULL;
    }
    UINT bytesRead;
    FRESULT result = f_read(sdState->img_file[driveNumber], buffer, bytesToRead, &bytesRead);
    if (FR_OK != result) {
        DBG_PRINTF("Failed to read the expected number of bytes");
        response->status = FILE_SEEK_ERROR;
        free(buffer);
        free(response->params);
        free(response);
        return NULL;
    }
    response->data_size = (uint16_t) bytesRead;
    response->data = buffer;
    response->status = STATUS_OK;
    create_command_crc8(response);
    create_data_crc8(response);
    
    return response;
}

Payload* sd_write(SDState *sdState, PIO_state *pio_state, Payload *payload) {
    ReadParams *writeParams = (ReadParams *)payload->params;

    Payload *response = (Payload*)malloc(sizeof(Payload));
    if (response == NULL) {
        printf("Error: Memory allocation failed for payload\n");
        return NULL;
    }
    memset(response, 0, sizeof(Payload));
    response->protocol = SD_BLOCK_DEVICE;
    response->command = WRITE_NO_VERIFY;
    response->params = (uint8_t *)malloc(1);
    if (response->params == NULL) {
        printf("Error: Memory allocation failed for response->params\n");
        free(response);
        return NULL;
    }
    response->params[0] = 0;

    int driveNumber = writeParams->drive_number;

    // Calculate the offset in the .img file
    int startSector = writeParams->start_sector;
    long offset = startSector * SECTOR_SIZE;

    // Move to the calculated offset
    if (FR_OK != f_lseek(sdState->img_file[driveNumber], offset)) {
        printf("Failed to seek to offset");
        response->status = FILE_SEEK_ERROR;
        free(response->params);
        free(response);
        return NULL;
    }

    // Calculate the number of bytes to write
    int sectorCount = writeParams->sector_count;
    size_t bytesToWrite = sectorCount * SECTOR_SIZE;

    UINT bytesWriten;
    FRESULT result = f_write(sdState->img_file[driveNumber], payload->data, bytesToWrite, &bytesWriten);
    if (FR_OK != result) {
        DBG_PRINTF("Failed to read the expected number of bytes");
        response->status = FILE_SEEK_ERROR;
        free(response->params);
        free(response);
        return NULL;
    }
    response->data_size = 1;
    response->data = (uint8_t *)malloc(1);
    response->data[0] = 0;
    response->status = STATUS_OK;
    create_command_crc8(response);
    create_data_crc8(response);
    
    return response;

}

Payload* create_error_response(SDState *sdState, PIO_state *pio_state, Payload *input) {
    Payload *response = (Payload*)malloc(sizeof(Payload));
    if (response == NULL) {
        printf("Error: Memory allocation failed for payload\n");
        return response;
    }
    memset(response, 0, sizeof(Payload));
    response->protocol = input->protocol;
    response->params_size = 0;
    response->command = input->command;
    response->status = input->status;
    response->params = NULL;
    response->data = NULL;
    create_command_crc8(response);
    create_data_crc8(response);
    return response;
}