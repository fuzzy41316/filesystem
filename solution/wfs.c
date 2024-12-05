#define FUSE_USE_VERSION 30
#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include "wfs.h"

///////////////////////
// Global Variables //
/////////////////////
typedef enum {
    RAID_UNKNOWN = -1,
    RAID_0 = 0,
    RAID_1 = 1,
    RAID_1V = 2
} raid_mode_t;

/* RAID Explanations:
 * RAID 0 (Striping):
 *   - RAID 0 applied to only data block
 *   - 1 data stripe is 512B (first 512B written to disk 1, the next 512B written to disk 2, and so on)
 * RAID 1 (Mirroring):
 *   - RAID 1 applied to superblock, inode bitmap, data bitmap, inode blocks, and data blocks
 * RAID 1V (Mirroring with Versioning):
 *   - RAID 1V applied to superblock, inode bitmap, data bitmap, inode blocks, and data blocks
 *   - Read operations compare all copies of data blocks on different drives and return data block present on majority of drives
 *   - If tie, data block on with lower mount position is returned
 */


struct wfs_sb *superblock = NULL;  // Contains metadata about filesystem
int raid_mode = -1; // RAID mode
int disk_count = 0; // Number of disks in filesystem
char *disk_files[MAX_DISKS]; // Disk file names
size_t disk_size[MAX_DISKS]; // Disk file sizes
void *disk_mmap[MAX_DISKS]; // Mapped disk files

///////////////////////////
// Function Definitions //
/////////////////////////

static int wfs_getattr(const char* path, struct stat* stbuf);
static int wfs_mknod(const char* path, mode_t mode, dev_t rdev);
static int wfs_mkdir(const char* path, mode_t mode);
static int wfs_unlink(const char* path);
static int wfs_rmdir(const char* path);
static int wfs_read(const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info* fi);
static int wfs_write(const char* path, const char* buf, size_t size, off_t offset, struct fuse_file_info* fi);
static int wfs_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi);

///////////////////////
// HELPER FUNCTIONS //
/////////////////////
int get_inode_num(const char *path);



int get_inode_num(const char *path)
{   
    // Make sure path is non-zero
    if (strlen(path) == 0)
        return 0;

    // Check if path is root
    if (strcmp(path, "/") == 0)
        return 0;   // Return 0 because root inode is always 0, and the offset is 0 when returned
    
    // Otherwise traverse the path /a/b/c for example to find the inode
    char *token;    // Delimiter for /
    char *path_copy = strdup(path); // Copy of path to modify
    char *final_slash = strrchr(path_copy, '/'); // Find final slash
    int inode_num = -1;  // Inode number to return

    // Assume superblock used is from first disk
    struct wfs_inode *inode = (struct wfs_inode *)((char *)disk_mmap[0] + superblock->i_blocks_ptr);

    while ((token = strtok(path_copy, "/")) != NULL)
    {
        // Find the inode number in the directory
        int found = 0;
        for (int i = 0; i < N_BLOCKS; i++)
        {   
            // Ignore if empty
            if (inode->blocks[i] == 0)
                continue;

            // Iterate through directory entries in the block
            struct wfs_dentry *dentry = (struct wfs_dentry *)((char *)disk_mmap[0] + inode->blocks[i]);
            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
            {
                // Entry matches name
                if (strcmp(dentry[j].name, token) == 0)
                {
                    // Make sure it's the last token (finalslash)
                    if (strcmp(final_slash + 1, dentry[j].name) == 0)
                    {
                        found = 1;
                        inode_num = dentry[j].num;
                        break;
                    }

                }
            }
            if (found)
                break;
        }

        // If not found, return error
        if (!found)
            return -ENOENT;
    }

    return inode_num;
}


//////////////////////////////
// END OF HELPER FUNCTIONS //
////////////////////////////

/////////////////////
// FUSE FUNCTIONS //
///////////////////

/* 
 * Return file attributes. The "stat" structure is described in detail in the stat(2) manual page. 
 * For the given pathname, this should fill in the elements of the "stat" structure. 
 * - st_uid
 * - st_gid
 * - st_atime
 * - st_mtime
 * - st_mode
 * - st_size
 */
static int wfs_getattr(const char* path, struct stat* stbuf)
{
    printf("wfs_getattr called with path: %s\n", path); // Debugging statement  

    printf("Getting inode number for path: %s\n", path);

    int inode_num = get_inode_num(path);
    if (inode_num < 0)
        return -ENOENT; // Inode does not exist
    
    printf("Successfully got inode number: %d\n", inode_num);

    // Get inode from the number
    struct wfs_inode *inode = (struct wfs_inode *)((char *)disk_mmap[0] + superblock->i_blocks_ptr + (inode_num * BLOCK_SIZE));

    stbuf->st_uid = inode->uid;
    stbuf->st_gid = inode->gid;
    stbuf->st_atime = inode->atim;
    stbuf->st_mtime = inode->mtim;
    stbuf->st_mode = inode->mode;
    stbuf->st_size = inode->size;

    return 0;
}

/* 
 * Make a special (device) file, FIFO, or socket. See mknod(2) for details. 
 * This function is rarely needed, since it's uncommon to make these objects inside special-purpose filesystems. 
 */
static int wfs_mknod(const char* path, mode_t mode, dev_t rdev)
{
    printf("wfs_mknod called with path: %s\n", path);       // Debugging statement
    return 0;
}

/* 
 * Create a directory with the given name. 
 * Directories may contain blank entries up to the size as marked in the directory inode. 
 * That is, a directory with size 512 bytes will use one data block, both of the following entry layouts would be legal -- 15 blank entries followed by a valid directory entry, 
 *      or a valid directory entry followed by 15 blank entries.
 * You should free all directory data blocks with rmdir, but you do not need to free directory data blocks when unlinking files in a directory.
 * A valid file/directory name consists of letters (both uppercase and lowercase), numbers, and underscores (_). 
 * Path names are always separated by forward-slash. You do not need to worry about escape sequences for other characters.
 */
static int wfs_mkdir(const char* path, mode_t mode)
{
    printf("wfs_mkdir called with path: %s\n", path);       // Debugging statement

    // Get the parents inode number from the path
    char parent_path[strlen(path) + 1];
    strcpy(parent_path, path);
    char *final_slash = strrchr(parent_path, '/');

    // There needs to be at least one parent directory (or root)
    if (final_slash == NULL)
        return -ENOENT; // No parent directory

    *final_slash = '\0';    // For null terminating the parent path

    int parent_inode_num = get_inode_num(parent_path);
    if (parent_inode_num < 0)
        return -ENOENT; // Parent inode does not exist
    
    // Make sure we can write to the parent before creating a directory under it
    struct wfs_inode *parent_inode_ptr = ((struct wfs_inode *)((char *)disk_mmap[0] + superblock->i_blocks_ptr + (parent_inode_num * BLOCK_SIZE)));

    if (!(parent_inode_ptr->mode & S_IWUSR))
        return -EACCES; // No write permission

    int new_inode_num = -1; // New inode number for the directory
    // Now find an empty inode if writable, by iterating through inodes in the superblock
    for (size_t i = 0; i < superblock->num_inodes; i++)
    {
        // Check if bitmap is set or not
        if ((((char *)(superblock->i_bitmap_ptr + (off_t)disk_mmap[0]))[i/8] & (1 << (i % 8))) != 0)
            continue;   // Inode is in use

        // Otherwise set the bitmap
        ((char *)(superblock->i_bitmap_ptr + (off_t)disk_mmap[0]))[i/8] |= (1 << (i % 8));
        new_inode_num = i;
    }

    if (new_inode_num == -1)
        return -ENOSPC; // No space for new inode

    // Now create a new inode for the directory
    struct wfs_inode *new_inode = ((struct wfs_inode *)((char *)disk_mmap[0] + superblock->i_blocks_ptr + (new_inode_num * BLOCK_SIZE)));
    new_inode->num = new_inode_num;
    new_inode->mode = mode | S_IFDIR; // Set mode to directory
    new_inode->nlinks = 1; // One link to itself initially
    new_inode->uid = getuid();
    new_inode->gid = getgid();
    new_inode->size = 0; // No size initially
    new_inode->atim = time(NULL);
    new_inode->mtim = time(NULL);
    new_inode->ctim = time(NULL);

    // Now add this inode to the directory 

    return 0;
}

/* 
 * Remove (delete) the given file, symbolic link, hard link, or special node. Note that if you support hard links, 
 * unlink only deletes the data when the last hard link is removed. See unlink(2) for details. 
 */
static int wfs_unlink(const char* path)
{
    printf("wfs_unlink called with path: %s\n", path);      // Debugging statement
    return 0;
}

/*
 * Remove the given directory. This should succeed only if the directory is empty (except for "." and ".."). See rmdir(2) for details.
 */
static int wfs_rmdir(const char* path)
{
    printf("wfs_rmdir called with given directory: %s\n", path);       // Debugging statement
    return 0;
}

/* 
 * Read sizebytes from the given file into the buffer buf, beginning offset bytes into the file. 
 * See read(2) for full details. Returns the number of bytes transferred, or 0 if offset was at or beyond the end of the file. 
 * Required for any sensible filesystem.
 */
static int wfs_read(const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info* fi)
{
    printf("wfs_read called with path: %s\n", path);      // Debugging statement
    return 0;
}

/* 
 * As for read above, except that it can't return 0.
 */
static int wfs_write(const char* path, const char* buf, size_t size, off_t offset, struct fuse_file_info* fi)
{
    printf("wfs_write called with path: %s\n", path);     // Debugging statement
    return 1;
}

/* 
 * The readdir function is somewhat like read, in that it starts at a given offset and returns results in a caller-supplied buffer. However, the offset not a byte offset, and the results are a series of struct dirents rather than being uninterpreted bytes. 
 * To make life easier, FUSE provides a "filler" function that will help you put things into the buffer.
 * The general plan for a complete and correct readdir is:
 *      1. Find the first directory entry following the given offset (see below).
 *      2. Optionally, create a struct stat that describes the file as for getattr (but FUSE only looks at st_ino and the file-type bits of st_mode).
 *      3. Call the filler function with arguments of buf, the null-terminated filename, the address of your struct stat (or NULL if you have none), and the offset of the next directory entry.
 *      4. If filler returns nonzero, or if there are no more files, return 0.
 *      5. Find the next file in the directory.
 *      6. Go back to step 2.
 * From FUSE's point of view, the offset is an uninterpreted off_t (i.e., an unsigned integer). You provide an offset when you call filler, and it's possible that such an offset might come back to you as an argument later. 
 * Typically, it's simply the byte offset (within your directory layout) of the directory entry, but it's really up to you.
 * It's also important to note that readdir can return errors in a number of instances; in particular it can return -EBADF if the file handle is invalid, or -ENOENT if you use the path argument and the path doesn't exist.
 */
static int wfs_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi)
{
    printf("wfs_readdir called with path: %s\n", path);   // Debugging statement
    return 0;
}


////////////////////////////
// END OF FUSE FUNCTIONS //
//////////////////////////

// FUSE operations structure
static struct fuse_operations ops = {
  .getattr = wfs_getattr,
  .mknod   = wfs_mknod,
  .mkdir   = wfs_mkdir,
  .unlink  = wfs_unlink,
  .rmdir   = wfs_rmdir,
  .read    = wfs_read,
  .write   = wfs_write,
  .readdir = wfs_readdir,
};

/*
 * ./wfs disk1 disk2 [FUSE options] mount_point
 * You need to pass [FUSE options] along with the mount_point to fuse_main as argv. You may assume -s is always passed to wfs as a FUSE option to disable multi-threading. 
 * We recommend testing your program using the -f option, which runs FUSE in the foreground. With FUSE running in the foreground, 
 * you will need to open a second terminal to test your filesystem. In the terminal running FUSE, printf messages will be printed to the screen. 
 * You might want to have a printf at the beginning of every FUSE callback so you can see which callbacks are being run.
 */
int main(int argc, char *argv[]) 
{
    printf("Starting main function...\n");

    // Need at least 2 disks and a mount point
    if (argc < 4)
    {
        printf("Usage: %s disk1 disk2 [FUSE options] mount_point\n", argv[0]);
        return 1;
    }
    
    // Count disks by checking if arguments are disk files
    disk_count = 0;
    int fuse_args_start = 1;
    printf("Counting disks...\n");
    while (fuse_args_start < argc && disk_count < MAX_DISKS)
    {
        // Check if argument starts with "-" (FUSE option)
        if (argv[fuse_args_start][0] == '-')
            break;  // Ignore FUSE options
        
        // Ignore mnt point
        if (strcmp(argv[fuse_args_start], argv[argc - 1]) == 0)
            break;

        // Store disk option
        disk_files[disk_count++] = argv[fuse_args_start++];
    }
    printf("Counting disk successful\n");

    // Open and mmap all disk files
    printf("Attempting to open and mmap disk files...\n");
    for (int i = 0; i < disk_count; i++)
    {
        // Open the disk for writing or reading
        int fd = open(disk_files[i], O_RDWR);
        if (fd == -1)
        {
            printf("Error opening file: %s\n", disk_files[i]);
            return 1;
        }

        // Get size of file for mmapping
        struct stat st;
        if (fstat(fd, &st) == -1)
        {
            printf("Error getting file size for file: %s\n", disk_files[i]);
            close(fd);
            return 1;
        }

        // Map the disk file into memory
        disk_size[i] = st.st_size;
        disk_mmap[i] = mmap(NULL, disk_size[i], PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (disk_mmap[i] == MAP_FAILED)
        {
            printf("Error mapping file into memory: %s\n", disk_files[i]);
            close(fd);
            return 1;
        }

        close(fd);  // Close file descriptor after mapping
    }
    printf("Disks opened and mmaped successfully\n");

    // Superblocks are mirrored across the disks, so just open from the first disk
    superblock = (struct wfs_sb *)disk_mmap[0];

    // Verify correct number of disks mounted
    if (disk_count != superblock->num_disks)
    {
        printf("Error: Wrong number of disks. Expected %d, got %d\n", superblock->num_disks, disk_count);
        return 1;
    }

    // Set RAID mode from superblock
    raid_mode = superblock->raid_mode;

    // Create new argv array for FUSE
    int fuse_argc = argc - fuse_args_start + 1; // +1 for argv[0]
    char **fuse_argv = malloc(fuse_argc * sizeof(char *));
    if (!fuse_argv)
    {
        printf("Error allocating memory for fuse_argv\n");
        return 1;
    }

    fuse_argv[0] = argv[0]; // Set the program name as the first argument

    // Copy FUSE arguments from argv to fuse_argv
    for (int i = fuse_args_start; i < argc; i++)
        fuse_argv[i - fuse_args_start + 1] = argv[i]; // +1 to skip argv[0]

    // Debug output
    printf("Superblock loaded:\n");
    printf("    RAID Mode: %d\n", superblock->raid_mode);
    printf("    # Inodes: %ld\n", superblock->num_inodes);
    printf("    # Data Blocks: %ld\n", superblock->num_data_blocks);
    printf("    Inode Bitmap Offset: %ld\n", superblock->i_bitmap_ptr);
    printf("    Data Bitmap Offset: %ld\n", superblock->d_bitmap_ptr);
    printf("    Inode Blocks Offset: %ld\n", superblock->i_blocks_ptr);
    printf("    Data Blocks Offset: %ld\n", superblock->d_blocks_ptr);
    printf("Mounting filesystem: Disks = %d, Mount point = %s\n", disk_count, argv[argc - 1]);


    // Start FUSE
    printf("Starting FUSE filesystem...\n");
    int ret = fuse_main(fuse_argc, fuse_argv, &ops, NULL);

    // Free the dynamically allocate fuse_argv
    free(fuse_argv);

    return ret;
}