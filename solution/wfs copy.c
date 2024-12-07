#define FUSE_USE_VERSION 30
#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
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
int current_disk = 0; // Current disk being accessed for RAID_0

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
static int allocate_data_block();
struct wfs_inode *allocate_inode(mode_t mode);
void set_inode_bitmap(char *inode_bitmap_ptr, size_t inode_num);
bool is_inode_set(char *inode_bitmap_ptr, size_t inode_num);
char** split_path(const char* path, int* count);
struct wfs_inode *get_inode_from_path(const char* path);

// Allocates a new data block using a bitmap, and returns the block number, or returns an error if there are no more blocks available
static int allocate_data_block()
{
    // Find an empty data block
    for (size_t i = 0; i < superblock->num_data_blocks; i++)
    {
        if (!is_inode_set((char *)disk_mmap[current_disk % disk_count] + superblock->d_bitmap_ptr, i))
        {
            set_inode_bitmap((char *)disk_mmap[current_disk % disk_count] + superblock->d_bitmap_ptr, i);

            return (int)(i * BLOCK_SIZE) + (int)superblock->d_blocks_ptr;
        }
    }

    return -ENOSPC; // No space left on device
}

// Allocates an inode using a bitmap, and returns the pointer to a new inode, or returns an error if there are no more nodes available
struct wfs_inode *allocate_inode(mode_t mode)
{
    // Find an empty inode
    int new_inode_num = -1;
    for (size_t i = 0; i < superblock->num_inodes; i++)
    {
        if (!is_inode_set((char *)disk_mmap[current_disk % disk_count] + superblock->i_bitmap_ptr, i))
        {
            set_inode_bitmap((char *)disk_mmap[current_disk % disk_count] + superblock->i_bitmap_ptr, i);
            new_inode_num = i;
            break;
        }
    }

    struct wfs_inode *new_inode = NULL;
    if (new_inode_num != -1)
        new_inode = (struct wfs_inode *)((char *)disk_mmap[current_disk % disk_count] + superblock->i_blocks_ptr + new_inode_num * BLOCK_SIZE);

    new_inode->num = new_inode_num;
    new_inode->mode = mode;
    new_inode->uid = getuid();
    new_inode->gid = getgid();
    new_inode->size = 0;    // New files are initially empty
    new_inode->nlinks = 1;  // New files have 1 link (themselves)
    new_inode->atim = time(NULL);
    new_inode->mtim = new_inode->atim;
    new_inode->ctim = new_inode->atim;

    return new_inode;
}

// Given the ptr to the inode_bitmap, set the bit in the inode_bitmap corresponding to the inode_num
void set_inode_bitmap(char *inode_bitmap_ptr, size_t inode_num)
{
    size_t byte_num = inode_num / 8;
    size_t bit_num = inode_num % 8;
    inode_bitmap_ptr[byte_num] |= 1 << bit_num;
}

// Given the ptr to the inode_bitmap, check if the bit in the inode_bitmap corresponding to the inode_num is set
bool is_inode_set(char *inode_bitmap_ptr, size_t inode_num)
{
    size_t byte_num = inode_num / 8;
    size_t bit_num = inode_num % 8;
    return inode_bitmap_ptr[byte_num] & (1 << bit_num);
}

// Function to split the path into components
char** split_path(const char* path, int* count) 
{   
    // Copy the path to avoid modifying the original
    char* path_copy = strdup(path);
    // Split the path into components
    char* token = strtok(path_copy, "/");
    char** components = malloc(sizeof(char*) * 10); 
    int i = 0;
    while (token != NULL) 
    {
        components[i++] = strdup(token);
        token = strtok(NULL, "/");
    }
    *count = i;
    free(path_copy);
    return components;
}

// Given a file path, traverse it to find the inode corresponding to the file
struct wfs_inode *get_inode_from_path(const char* path)
{
    int count;
    char** components = split_path(path, &count);

    // Start at the root inode
    struct wfs_inode *current_inode = (struct wfs_inode *)((char *)disk_mmap[current_disk % disk_count] + superblock->i_blocks_ptr);

    for (int i = 0; i < count; i++)
    {
        if (S_ISDIR(current_inode->mode))
        {
            int found = 0;
            struct wfs_dentry *dentry;

            for (int j = 0; j < D_BLOCK; j++)
            {
                if (current_inode->blocks[j] != 0)
                {
                    dentry = (struct wfs_dentry*)((char*)disk_mmap[current_disk % disk_count] + current_inode->blocks[j] * BLOCK_SIZE);
                    
                    for (int k = 0; k < BLOCK_SIZE / sizeof(struct wfs_dentry); k++)
                    {
                        if (strcmp(dentry[k].name, components[i]) == 0)
                        {
                            current_inode = (struct wfs_inode*)((char*)disk_mmap[current_disk % disk_count] + superblock->i_blocks_ptr + dentry[k].num * BLOCK_SIZE);
                            found = 1;
                            break;
                        }
                    }
                }
                if (found)
                    break;
            }
            if (!found)
            {
                current_inode = NULL;
                break;
            }
        }
    }

    for (int i = 0; i < count; i++)
        free(components[i]);
    free(components);

    return current_inode;   
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

    struct wfs_inode *inode = get_inode_from_path(path);

    if (inode == NULL)
        return -ENOENT; // File not found

    // Fill in the stat structure
    stbuf->st_uid = inode->uid;
    stbuf->st_gid = inode->gid;
    stbuf->st_atime = inode->atim;
    stbuf->st_mtime = inode->mtim;
    stbuf->st_mode = inode->mode;
    stbuf->st_size = inode->size;

    // Print out the inode stats for debugging purposes
    printf("Inode stats:\n");
    printf("----------------\n");
    printf("    UID: %d\n", inode->uid);
    printf("    GID: %d\n", inode->gid);
    printf("    Atime: %ld\n", inode->atim);
    printf("    Mtime: %ld\n", inode->mtim);
    printf("    Ctime: %ld\n", inode->ctim);
    printf("    Mode: %d\n", inode->mode);
    printf("    Size: %ld\n", inode->size);
    printf("----------------\n");

    return 0;
}

   /* create foo/bar
     * 1. read root inode
     * 2. read root data
     * 3. read foo inode
     * 4. read foo data
     * 5. read inode_bitmap
     * 6. write inode_bitmap
     * 7. write foo data
     * 8. read bar inode
     * 9. write bar inode
     * 10. write foo inode
     * 
     * In essence:
     * 1. Ensure foo/bar doesn't already exist
     * 2. Find unused inode & write out updated inode bitmap
     * 3. initialize bar inode and write it back
     * 4. update modified time of foo inode
     * 
     */

    /* Furthermore to create a file 
     * 1. Allocate a new inode using the inode bitmap
     * 2. Then add a directory entry to the parent inode, the inode of the directory containing the file
     * 3. New files are initially empty and have a size of 0 bytes
     */
/* 
 * Make a special (device) file, FIFO, or socket. See mknod(2) for details. 
 * This function is rarely needed, since it's uncommon to make these objects inside special-purpose filesystems. 
 */
static int wfs_mknod(const char* path, mode_t mode, dev_t rdev)
{
    printf("wfs_mknod called with path: %s\n", path);       // Debugging statement

    // Ensure that the file doesn't already exist
    struct wfs_inode *file_inode = get_inode_from_path(path);
    if (file_inode != NULL)
        return -EEXIST; // File already exists
    
    // Find parent directory
    int count;
    char** components = split_path(path, &count);
    char *parent_path = malloc(strlen(path) + 1);
    strcpy(parent_path, path);
    parent_path[strlen(path) - strlen(components[count - 1]) - 1] = '\0'; // Remove the last component from the path
    struct wfs_inode *parent_inode = get_inode_from_path(parent_path);
    free(parent_path);

    if (parent_inode == NULL)
        return -ENOENT; // Parent directory doesn't exist

    // Make sure parent directory is able to be written to
    if (!(parent_inode->mode & S_IWUSR))
        return -EACCES; // No write permission
    
    // Find an empty inode and create a new entry in the parent directory
    struct wfs_inode *new_inode = allocate_inode(mode);
    if (new_inode == NULL)
        return -ENOSPC; // No space left for a new inode

    // Add new entry to parent directory
    struct wfs_dentry *dentry;
    int found = -1;

    for (int i = 0; i < N_BLOCKS; i++)
    {
        // Check if data block exists
        if (parent_inode->blocks[i] != 0)
        {
            printf("Data block exists\n");
            dentry = (struct wfs_dentry*)((char*)disk_mmap[current_disk % disk_count] + parent_inode->blocks[i] * BLOCK_SIZE);

            // Iterate through the dentry to find an empty entry
            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
            {
                if (dentry[j].num == 0)
                {
                    if (strncpy(dentry[j].name, components[count - 1], MAX_NAME) == NULL)
                        return -ENAMETOOLONG; // File name too long

                    dentry[j].num = new_inode->num;
                    found = 1;
                    break;
                    printf("Entry added to parent directory\n");
                }
            }
        if (found)
            break;
        }
        // If no data block exists, allocate a new data block for the inode
        else
        {   
            printf("Data block does not exist\n");

            off_t block_num = allocate_data_block();
            if (block_num < 0)
                return -ENOSPC; // No space left on device

            printf("Data block allocated\n");

            // Add new entry to parent directory
            parent_inode->blocks[i] = block_num;
            dentry = (struct wfs_dentry*)((char*)disk_mmap[current_disk % disk_count] + block_num* BLOCK_SIZE);

            if (strncpy(dentry[0].name, components[count - 1], MAX_NAME) == NULL)
                return -ENAMETOOLONG; // File name too long
            dentry[0].num = new_inode->num;
            found = 1;
            printf("Entry added to parent directory\n");
        }
        if (found)
            break;
    }
    if (found < 0)
        return -ENOSPC; // No space left on device
    
    // Update parent inode
    parent_inode->mtim = time(NULL);

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

    printf("Checking if file exists...\n");
    // Ensure that the file doesn't already exist
    struct wfs_inode *file_inode = get_inode_from_path(path);
    if (file_inode != NULL)
        return -EEXIST; // File already exists
    printf("File does not exist\n");
    
    // Find parent directory
    printf("Finding parent directory...\n");
    char parent_path[strlen(path) + 1];
    strcpy(parent_path, path);
    char *final_slash = strrchr(parent_path, '/');
    *final_slash = '\0'; // Remove the last component from the path
    printf("Parent directory found: %s\n", parent_path);

    struct wfs_inode *parent_inode = get_inode_from_path(parent_path);

    if (parent_inode == NULL)
        return -ENOENT; // Parent directory doesn't exist

    // Make sure parent directory is able to be written to
    if (!(parent_inode->mode & S_IWUSR))
        return -EACCES; // No write permission
    
    printf("Checking for an empty inode...\n");
    // Find an empty inode and create a new entry in the parent directory
    struct wfs_inode *new_inode = allocate_inode(mode & S_IFDIR);
    if (new_inode == NULL)
        return -ENOSPC; // No space left for a new inode

    // Add new entry to parent directory
    struct wfs_dentry *dentry;
    for (int i = 0; i < D_BLOCK; i++)
    {
        if (parent_inode->blocks[i] != 0)
        {
            dentry = (struct wfs_dentry*)((char*)disk_mmap[current_disk % disk_count] + parent_inode->blocks[i] * BLOCK_SIZE);
            for (int j = 0; j < BLOCK_SIZE / sizeof(struct wfs_dentry); j++)
            {
                if (dentry[j].num == 0)
                {
                    if (snprintf(dentry[j].name, MAX_NAME, "%s", final_slash + 1) >= MAX_NAME)
                        return -ENAMETOOLONG; // Name too long
                    dentry[j].num = new_inode->num;
                    break;
                }
            }
        }
        else
        {
            parent_inode->blocks[i] = allocate_data_block();
            dentry = (struct wfs_dentry*)((char*)disk_mmap[current_disk % disk_count] + parent_inode->blocks[i] * BLOCK_SIZE);
            if (snprintf(dentry[0].name, MAX_NAME, "%s", final_slash + 1) >= MAX_NAME)
                return -ENAMETOOLONG; // Name too long
            dentry[0].num = new_inode->num;
            break;
        }
    }

    // Update parent inode
    parent_inode->mtim = time(NULL);

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

    return 1;
}

/* 
 * As for read above, except that it can't return 0.
 */
static int wfs_write(const char* path, const char* buf, size_t size, off_t offset, struct fuse_file_info* fi)
{
    printf("wfs_write called with path: %s\n", path);     // Debugging statement

    /* Write to to foo/bar
     * 1. Make sure file exists
     * 2. read bar inode for list of current data blocks and how large file is
     * 3. If there's space in current data block, then use it, otherwise allocate a new data block
     *  3a. use the data_bitmap to allocate a new datablock
     * 4. Write bar data to data block
     * 5. Update timestamp and address of new data block inside the bar inode
    */

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