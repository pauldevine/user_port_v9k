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
#include "v9k_hard_drives.h"

static const bool DEBUG_SDIO = false;

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

uint64_t calculate_mbr_offset(uint32_t partition_start_lba, uint32_t sector_number_within_partition, uint32_t sector_size) {
    return (uint64_t)(partition_start_lba + sector_number_within_partition) * sector_size;
}

uint64_t calculate_victor_offset(
    uint32_t logical_sector, // Logical sector number within the virtual volume
    MediaList *working_media_list,     // Media list describing the working regions
    uint32_t sector_size                  // Sector size in bytes
) {
    uint64_t offset = 0;

    // Iterate over the regions to map the logical sector to a physical sector
    for (uint8_t i = 0; i < working_media_list->num_regions; i++) {
        Region *region = &working_media_list->regions[i];
        uint32_t region_size = region->region_size;

        // Check if the logical sector falls within this region
        if (logical_sector < region_size) {
            // Logical sector is within this region; calculate physical sector
            uint32_t physical_sector = region->region_size + logical_sector;

            // Calculate byte offset
            offset = (uint64_t)physical_sector * sector_size;
            return offset;
        } else {
            // Logical sector is beyond this region; move to the next region
            logical_sector -= region_size;
        }
    }

    // If we exhaust the regions without finding a match, return an error indicator
    printf("Error: Logical sector %u is out of bounds\n", logical_sector);
    return 0xFFFFFFFFFFFFFFFF; // Return a large invalid offset to indicate failure
}

int read_mbr(FIL *disk_image, MBR *mbr) {
    FRESULT res;

    // Read the MBR (first 512 bytes)
    size_t bytes_read;
    res = f_read(disk_image, mbr, sizeof(MBR), &bytes_read);
    if (bytes_read != sizeof(MBR)) {
        perror("Error reading MBR");
        return -1;
    }

    return 0;
}

int read_bpb(FIL *disk_image, uint32_t start_lba, BPB_FAT12 *bpb) {
    FRESULT res;
    UINT bytes_read;

    // Calculate the byte offset to the start of the first partition
    FSIZE_t offset = (FSIZE_t)start_lba * SECTOR_SIZE;
    if (DEBUG_SDIO) { printf("Reading MBR at offset %lu\n", offset); }
    
    res = f_lseek(disk_image, offset);
    // Seek to the beginning of the first partition
    if (res != FR_OK) {
        perror("Error seeking to first partition");
        return -1;
    }

    // Read the BPB (first sector of the partition)
    res = f_read(disk_image, bpb, sizeof(BPB_FAT12), &bytes_read);
    if (res != FR_OK) {
        printf("f_read failed: %d\n", res);
        return -1;
    }
    if (bytes_read != sizeof(BPB_FAT12)) {
        perror("Error reading BPB");
        return -1;
    }

    return 0;
}


// Function to read a sector from the image file
int read_sector(FIL *img_file, uint32_t partition_start_lba, uint32_t sector_number, uint8_t *buffer) {
    FRESULT res;
    UINT bytes_read;

    // Move the file pointer to the desired sector
    FSIZE_t offset = (FSIZE_t) calculate_mbr_offset(partition_start_lba, sector_number, SECTOR_SIZE);
    if (DEBUG_SDIO) { printf("Reading sector %u at offset %lu\n", sector_number, offset); }
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

int read_fat12_bpb_from_img_file(DriveImage *drive_image, VictorBPB *victor_bpb) {
    MBR mbr;
    BPB_FAT12 bpb;
    FIL *img_file = drive_image->img_file;

    // Read the MBR
    if (read_mbr(img_file, &mbr) != 0) {
        fprintf(stderr, "Failed to read MBR\n");
        return -1;
    }

    // Check the boot signature
    if (mbr.boot_signature != 0xAA55) {
        fprintf(stderr, "Invalid MBR boot signature: 0x%X\n", mbr.boot_signature);
        return -1;
    }

    // Get the first partition entry
    PartitionEntry *first_partition = &mbr.partition_table[0];

    if (first_partition->partition_type == 0) {
        fprintf(stderr, "First partition is not defined.\n");
        return -1;
    }

    if (DEBUG_SDIO) {
        printf("First Partition Details:\n");
        printf("  Boot Indicator: 0x%X\n", first_partition->boot_indicator);
        printf("  Partition Type: 0x%X\n", first_partition->partition_type);
        printf("  Start LBA: %u\n", first_partition->start_lba);
        printf("  Size in Sectors: %u\n\n", first_partition->size_in_sectors);
    }

    // Read the BPB from the first partition
    if (read_bpb(img_file, first_partition->start_lba, &bpb) != 0) {
        fprintf(stderr, "Failed to read BPB from the first partition\n");
        return -1;
    }

    victor_bpb->bytes_per_sector = bpb.bytes_per_sector;
    victor_bpb->sectors_per_cluster = bpb.sectors_per_cluster;
    victor_bpb->reserved_sectors = bpb.reserved_sector_count;
    victor_bpb->num_fats = bpb.num_fats;
    victor_bpb->root_entry_count = bpb.root_entry_count;
    victor_bpb->total_sectors = bpb.total_sectors_16;
    victor_bpb->media_descriptor = bpb.media_type;
    victor_bpb->sectors_per_fat = bpb.fat_size_16;
    
    drive_image->start_lba = first_partition->start_lba;
    drive_image->end_lba = first_partition->start_lba + first_partition->size_in_sectors;

    print_debug_bpb(victor_bpb);

    return 0;
}

void print_debug_bpb(VictorBPB *bpb) {
    printf("BIOS Parameter Block (BPB) Information:\n");
    printf("  Bytes per Sector: %u\n", bpb->bytes_per_sector);
    printf("  Sectors per Cluster: %u\n", bpb->sectors_per_cluster);
    printf("  Reserved Sector Count: %u\n", bpb->reserved_sectors);
    printf("  Number of FATs: %u\n", bpb->num_fats);
    printf("  Root Entry Count: %u\n", bpb->root_entry_count);
    printf("  Total Sectors: %u\n", bpb->total_sectors);
    printf("  Media Descriptor: 0x%X\n", bpb->media_descriptor);
    printf("  Sectors per FAT: %u\n", bpb->sectors_per_fat);
}

// Function to print the disk label and lists
void print_v9k_disk_label(const V9kDriveLabel *label, const MediaList *available_media, const MediaList *working_media, const VolumeList *virtual_volume, const VirtualVolumeLabel *volume_labels) {
    printf("Disk Label:\n");
    printf("  Label Type: %d\n", label->label_type);
    printf("  Device ID: %d\n", label->device_id);
    printf("  Serial Number: %.16s\n", label->serial_number);
    printf("  Sector Size: %d\n", label->sector_size);
    printf("  Disk Address: %u\n", label->disk_address);
    printf("  Load Address: %u\n", label->load_address);
    printf("  Load Length: %u\n", label->load_length);
    printf("  Code Entry: %u\n", label->code_entry);
    printf("  Primary Boot Volume: %d\n", label->primary_boot_volume);
    printf("  Number of Cylinders: %d\n", label->num_cylinders);
    printf("  Number of Heads: %d\n", label->num_heads);
    printf("  First RW Cycle: %d\n", label->first_rw_reduced);
    printf("  First Write Precomp: %d\n", label->first_write_precomp);
    printf("  ECC Burst Length: %d\n", label->ecc_burst_length);
    printf("  Fast Step Control: %d\n", label->fast_step_control);
    printf("  Interleave Factor: %d\n", label->interleave_factor);

    printf("\nAvailable Media List (%d regions):\n", available_media->num_regions);
    for (int i = 0; i < available_media->num_regions; i++) {
        printf("  Region %d: Address = %u, Size = %u\n", i,
               available_media->regions[i].physical_address,
               available_media->regions[i].region_size);
    }

    printf("\nWorking Media List (%d regions):\n", working_media->num_regions);
    for (int i = 0; i < working_media->num_regions; i++) {
        printf("  Region %d: Address = %u, Size = %u\n", i,
               working_media->regions[i].physical_address,
               working_media->regions[i].region_size);
    }

    printf("\nVirtual Volume List (%d volumes):\n", virtual_volume->num_volumes);
    for (int i = 0; i < virtual_volume->num_volumes; i++) {
        printf("  Volume %d: Logical Address = %u\n", i, virtual_volume->volume_addresses[i]);
    }

    printf("\nVirtual Volume Labels:\n");
    printf("  Volume Name: %.16s\n", volume_labels->volume_name);
    printf("  Label Type: %d\n", volume_labels->label_type);
    printf("  Disk Address: %u\n", volume_labels->disk_address);
    printf("  Load Address: %u\n", volume_labels->load_address);
    printf("  Load Length: %u\n", volume_labels->load_length);
    printf("  Code Entry: %u\n", volume_labels->code_entry);
    printf("  Volume Capacity: %u\n", volume_labels->volume_capacity);
    printf("  Data Start: %u\n", volume_labels->data_start);
    printf("  Host Block Size: %u\n", volume_labels->host_block_size);
    printf("  Allocation Unit: %d\n", volume_labels->allocation_unit);
    printf("  Directory Entries: %u\n", volume_labels->directory_entries);
    printf("  Assignment Count: %u\n", volume_labels->assignment_count);
    for (int i=0; i<volume_labels->assignment_count; i++) {
        printf("  Assignment %d: %u\n", i, volume_labels->assignments[i]);
    }
}


/* Function to parse the BPB from a FAT16 .img file */
int parse_fat16_bpb(DriveImage *drive_image, VictorBPB *bpb) {

    FRESULT res;
    uint8_t buffer[SECTOR_SIZE];
    FIL *img_file = drive_image->img_file;

    uint8_t boot_sector[SECTOR_SIZE]; 
    size_t bytes_read;

    // Read MBR from sector 0
    if (read_sector(img_file, drive_image->start_lba, 0, buffer) != 0) {
        f_close(img_file);
        return -1;
    }

     // Get the starting sector of the first partition
    uint32_t partition_start = get_first_partition_start(buffer);
    if (DEBUG_SDIO) { printf("First partition starts at sector: %u\n", partition_start); }

    // Read the boot sector of the first partition
    if (read_sector(img_file, drive_image->start_lba, partition_start, boot_sector) != 0) {
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
   
    print_debug_bpb(bpb);
    return 0; /* Success */
}
// Function to parse the BPB from a Victor 9000 .img file
// Returns the number of virtual volumes found
// img_num is the index of the image file in the array of DriveImage pointers
// drive_image is an array of DriveImage pointers
// bpb is an array of VictorBPB structures
// max_units is the maximum number of virtual volumes we support
int build_bpbs_from_v9k_disk_label(uint8_t img_num, DriveImage *drive_image[], VictorBPB *bpb, uint8_t max_units) {
    
    uint8_t result;
    int vol;
    size_t bytes_read;
    FIL *img_file = drive_image[img_num]->img_file;

    // Read and parse the drive label
    V9kDriveLabel drive_label = {0};
    result = read_drive_label(img_file, &drive_label); 
    if (result != 0) {
        printf("Error parsing variable lists\n");
        return -1;
    }

    // Parse variable drive metadata lists
    MediaList available_media_list = {0};
    MediaList working_media_list = {0};
    VolumeList volume_list = {0};

    result = parse_media_list(img_file, &available_media_list);
    if (result != 0) {
        printf("Error parsing available_media_list\n");
        return -1;
    }

    result = parse_media_list(img_file, &working_media_list);
    if (result != 0) {
        printf("Error parsing working_media_list\n");
        return -1;
    }

    result = parse_volume_list(img_file, &volume_list);
    if (result != 0) {
        printf("Error parsing volume_list\n");
        return -1;
    }
    
    // Populate BPBs for each virtual volume
    for (vol = 0; vol < volume_list.num_volumes; vol++) {
        if (img_num >= max_units) {
            printf("Reached maximum number of units\n");
            return vol;
        }
        VirtualVolumeLabel volume_label = {0};
        result = read_virtual_volume_label(img_file, volume_list.volume_addresses[vol], &volume_label);
        if (result != 0) {
            printf("Error reading virtual volume label\n");
            return vol;
        }
        if (volume_label.label_type == 65535) {
            continue; // Skip maintenance volume entries
        }

        uint32_t start_lba = volume_list.volume_addresses[vol];
        drive_image[img_num]->start_lba = start_lba;
        drive_image[img_num]->end_lba = start_lba + volume_label.volume_capacity - 1;

        /* Populate the BPB structure */
        bpb[vol].bytes_per_sector = volume_label.host_block_size;
        bpb[vol].sectors_per_cluster = volume_label.allocation_unit;
        bpb[vol].reserved_sectors = volume_label.data_start; // First volumes have parition info, others start at 0.
        bpb[vol].num_fats = 2; // Standard value
        bpb[vol].root_entry_count = volume_label.directory_entries;
        bpb[vol].total_sectors = (uint16_t)volume_label.volume_capacity;
        bpb[vol].media_descriptor = 0xF8; // Standard hard disk media descriptor

        // Calculate sectors per FAT
        uint32_t root_dir_sectors = (volume_label.directory_entries * 32 + drive_label.sector_size - 1) / drive_label.sector_size;
        uint32_t data_sectors = volume_label.volume_capacity - (bpb[vol].reserved_sectors + root_dir_sectors);
        uint32_t total_clusters = data_sectors / volume_label.allocation_unit;

        // FAT size depends on total clusters (assume FAT16 for simplicity)
        //bpb[vol].sectors_per_fat = (total_clusters * 2 + drive_label.sector_size - 1) / drive_label.sector_size;
        bpb[vol].sectors_per_fat = 11;

        print_v9k_disk_label(&drive_label, &available_media_list, &working_media_list, &volume_list, &volume_label);
        print_debug_bpb(&bpb[vol]);
        img_num++;
    }

    uint8_t volumes_found = vol + 1;

    return volumes_found; // count of volumes instantiated
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

    if (DEBUG_SDIO) { printf("Mounted SD card\n"); }
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
       if (DEBUG_SDIO) { printf("directory entry: %s\n", fno.fname); }
        if (fno.fname[0] == 0 || sdState->fileCount >= MAX_IMG_FILES) {
            if (DEBUG_SDIO) { printf("No more files\n"); }
            break;
        }
        if (matches_pattern(fno.fname)) {
            printf("Found matching file: %d %s\n", sdState->fileCount, fno.fname);
            strncpy(sdState->file_names[sdState->fileCount], fno.fname, FILENAME_MAX_LENGTH - 1);
            //sdState->file_names[sdState->fileCount][FILENAME_MAX_LENGTH - 1] = '\0';
            sdState->images[sdState->fileCount] = malloc(sizeof(DriveImage));
            sdState->images[sdState->fileCount]->img_file = malloc(sizeof(FIL));
            if (!sdState->images[sdState->fileCount]->img_file) {
                perror("Failed to allocate FIL");
                // Handle cleanup and error
            }
            FRESULT fr = f_open(sdState->images[sdState->fileCount]->img_file, fno.fname, FA_OPEN_EXISTING | FA_READ | FA_WRITE);
            if (FR_OK != fr) {
                printf("error opening file, %s", fno.fname);
            }
            sdState->fileCount++;
        }
    }
    f_closedir(&dir);
    if (DEBUG_SDIO) { printf("file list length: %d\n", sdState->fileCount); }

    // Loop through and print each string
    for (int i = 0; i < sdState->fileCount; ++i) {
        printf("sdState->file_names[%s]: \n", sdState->file_names[i]);
    }
    return sdState;
}

void freeSDState(SDState *sdState) {
    for (int i = 0; i < sdState->fileCount; i++) {
        f_close(sdState->images[i]->img_file);
    }
    f_close(sdState->debug_log);
    f_unmount("");
    free(sdState->fs);
    free(sdState);
}


//returns an arraay of BPBs used to initialize the drives
Payload* init_sd_card(SDState *sdState, PIO_state *pio_state, Payload *payload) {

    if (DEBUG_SDIO) { printf("Found %d matching files:\n", sdState->fileCount); }
    for (int i = 0; i < sdState->fileCount; i++) {
       if (DEBUG_SDIO) { printf("%s\n", sdState->file_names[i]); }
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
    uint8_t i=0;
    while (i < num_drives) {
        printf("Parsing BPB for %s\n", sdState->file_names[i]);
        if (strcasestr(sdState->file_names[i], "_v9k") != 0) {
            //each V9k disk image has multiple volumes, so we need to build BPBs for each volume
            uint8_t volumes = build_bpbs_from_v9k_disk_label(i, sdState->images, initPayload->bpb_array, (MAX_IMG_FILES - i));
            if (volumes == 0) {
                printf("Error parsing BPB for %s\n", sdState->file_names[i]);
            }
            i += volumes;
        } else {
            if (read_fat12_bpb_from_img_file(sdState->images[i], &initPayload->bpb_array[i]) != 0) {
                printf("Error parsing BPB for %s\n", sdState->file_names[i]);
            }
            i++;
        }
    }

    for (int i = 0; i < num_drives; i++) {
       if (DEBUG_SDIO) { printf("BPB for drive %d %c %s\n", i, (i + 'C'), sdState->file_names[i]); }
        print_debug_bpb(&initPayload->bpb_array[i]);
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
    long offset = calculate_mbr_offset(sdState->images[driveNumber]->start_lba, startSector, SECTOR_SIZE);
    if (DEBUG_SDIO) { printf("sd_read startSector: %u, Offset: %ld\n", startSector, offset); }

    // Move to the calculated offset
    if (FR_OK != f_lseek(sdState->images[driveNumber]->img_file, offset)) {
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
    FRESULT result = f_read(sdState->images[driveNumber]->img_file, buffer, bytesToRead, &bytesRead);
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
    
    if (DEBUG_SDIO) {
        printf("sd_read Read %u bytes\n", bytesRead);
        printf("sd_read Read %.40s\n", response->data);
        printf("sd_read Read: ");

        for (int i = 0; i < 40; i++) {
            printf(" %x", response->data[i]);
        }
        printf("\n");
    }

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
    FSIZE_t offset = (FSIZE_t) calculate_mbr_offset(sdState->images[driveNumber]->start_lba, startSector, SECTOR_SIZE);
    if (DEBUG_SDIO) { printf("sd_write startSector: %u, Offset: %ld\n", startSector, offset); }

    // Move to the calculated offset
    if (FR_OK != f_lseek(sdState->images[driveNumber]->img_file, offset)) {
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
    FRESULT result = f_write(sdState->images[driveNumber]->img_file, payload->data, bytesToWrite, &bytesWriten);
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

    if (DEBUG_SDIO) {
        printf("sd_write Wrote %u bytes\n", bytesWriten);
        printf("sd_write Wrote %.40s\n", payload->data);
        printf("sd_write Wrote: ");
        for (int i = 0; i < 40; i++) {
            printf(" %x", payload->data[i]);
        }
        printf("\n");
    }

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