#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include "wfs.h"

// RAID modes
typedef enum {
    RAID_UNKNOWN = -1,
    RAID_0 = 0,
    RAID_1 = 1,
    RAID_1V = 2
} raid_mode_t;

/*
 * This C program initializes a file to an empty filesystem. I.e. to the state, where the filesystem can be mounted and other files and directories can be created under the root inode. 
 * The program receives three arguments: the raid mode, disk image file (multiple times), the number of inodes in the filesystem, and the number of data blocks in the system. 
 * The number of blocks should always be rounded up to the nearest multiple of 32 to prevent the data structures on disk from being misaligned. 
 * For example:
 * ./mkfs -r 1 -d disk1 -d disk2 -i 32 -b 200
 * initializes all disks (disk1 and disk2) to an empty filesystem with 32 inodes and 224 data blocks. The size of the inode and data bitmaps are determined by the number of blocks specified by mkfs. 
 * If mkfs finds that the disk image file is too small to accommodate the number of blocks, it should exit with return code -1. mkfs should write the superblock and root inode to the disk image.
 * 
 */
int main(int argc, char *argv[]) 
{
    // Acquire arguments 
    int raid_mode = -1;
    int num_inodes = 0;
    int num_data_blocks = 0;
    char *disk_files[MAX_DISKS];
    int disk_count = 0;

    // Process arguments
    for (int i = 1; i < argc; i++) 
    {
        if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) 
        {
            if (strcmp(argv[i + 1], "0") == 0) raid_mode = RAID_0;
            else if (strcmp(argv[i + 1], "1") == 0) raid_mode = RAID_1;
            else if (strcmp(argv[i + 1], "1v") == 0) raid_mode = RAID_1V;
            else return 1; // Invalid raid mode     
            i++;
        } 
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) 
        {
            disk_files[disk_count++] = argv[++i]; // Increment i after consuming the value
        } 
        else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) 
        {
            num_inodes = atoi(argv[++i]); // Increment i after consuming the value
        } 
        else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) 
        {
            num_data_blocks = atoi(argv[++i]); // Increment i after consuming the value
        } 
        else 
        {
            fprintf(stderr, "Usage: %s -r <raid_mode> -d <disk1> [-d <disk2> ...] -i <inodes> -b <blocks>\n", argv[0]);
            return 1;
        }
    }
    
    // Check if arguments are valid
    if (raid_mode == -1 || disk_count == 0 || num_inodes == 0 || num_data_blocks == 0) return 1;
     
    // All supported raid modes require 2 or more disks
    if (disk_count < 2) return 1;

    // Round up the number of inodes and data blocks to the nearest multiple of 32
    if (num_inodes % 32 != 0) num_inodes = num_inodes + 32 - (num_inodes % 32);
    if (num_data_blocks % 32 != 0) num_data_blocks = num_data_blocks + 32 - (num_data_blocks % 32);

    // Calculate bitmap sizes for inodes and data blocks
    size_t i_bitmap_size = num_inodes / 8;
    size_t d_bitmap_size = num_data_blocks / 8;

    // Calculate offsets
    off_t i_bitmap_offset = sizeof(struct wfs_sb);
    off_t d_bitmap_offset = i_bitmap_offset + i_bitmap_size;
    off_t inode_region_offset = ((d_bitmap_offset + d_bitmap_size) % BLOCK_SIZE == 0) ? d_bitmap_offset + d_bitmap_size : (d_bitmap_offset + d_bitmap_size) + (BLOCK_SIZE - (d_bitmap_offset + d_bitmap_size) % BLOCK_SIZE);
    off_t data_region_offset = inode_region_offset + num_inodes * BLOCK_SIZE;

    // Initialize superblock
    struct wfs_sb sb;
    sb.num_inodes = num_inodes;
    sb.num_data_blocks = num_data_blocks;
    sb.i_bitmap_ptr = i_bitmap_offset;
    sb.d_bitmap_ptr = d_bitmap_offset;
    sb.i_blocks_ptr = inode_region_offset;
    sb.d_blocks_ptr = data_region_offset;
    sb.raid_mode = raid_mode;
    sb.num_disks = disk_count;

    for (int i = 0; i < disk_count; i++)
    {
        strncpy(sb.disk_order[i], disk_files[i], MAX_NAME - 1);
        sb.disk_order[i][MAX_NAME - 1] = '\0';
    }

    // Allocate and initialize bitmaps for inodes and data blocks
    char *ibitmap = calloc(1, i_bitmap_size);
    char *dbitmap = calloc(1, d_bitmap_size);

    // Mark root inode as allocated
    ibitmap[0] |= 1;

    // Initialize root inode
    struct wfs_inode root_inode;
    memset(&root_inode, 0, sizeof(struct wfs_inode));
    root_inode.num = 0;
    root_inode.mode = S_IFDIR | 0755;
    root_inode.uid = getuid();
    root_inode.gid = getgid();
    root_inode.size = 0;
    root_inode.nlinks = 2;
    time(&root_inode.atim);
    root_inode.mtim = root_inode.atim;
    root_inode.ctim = root_inode.atim;

    // Now write to each disk
    for (int d = 0; d < disk_count; d++)
    {   
        // Open the disk file
        int fd = open(disk_files[d], O_RDWR);
        if (fd < 0)
        {
            close(fd);
            return 255;
        }

        // Check if disk is large enough
        struct stat st;
        if (fstat(fd, &st) < 0)
        {
            close(fd);
            return 255;
        }
        off_t required_size = data_region_offset + num_data_blocks * BLOCK_SIZE;
        if (st.st_size < required_size)
        {
            close(fd);
            return 255;
        }

        // Write superblock
        if (pwrite(fd, &sb, sizeof(struct wfs_sb), 0) != sizeof(struct wfs_sb))
        {
            close(fd);
            return 255;
        }

        // Write inode bitmap
        if (pwrite(fd, ibitmap, i_bitmap_size, i_bitmap_offset) != i_bitmap_size)
        {
            close(fd);
            return 255;
        }

        // Write data block bitmap
        if (pwrite(fd, dbitmap, d_bitmap_size, d_bitmap_offset) != d_bitmap_size)
        {
            close(fd);
            return 255;
        }

        // Write root inode
        off_t root_inode_offset = inode_region_offset + root_inode.num * BLOCK_SIZE;
        if (pwrite(fd, &root_inode, sizeof(struct wfs_inode), root_inode_offset) != sizeof(struct wfs_inode))
        {
            close(fd);
            return 255;
        }
        close(fd);
    }

    // Free resources
    free(ibitmap);
    free(dbitmap);
    return 0;
}