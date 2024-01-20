#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include "p4Heap.h"
 
/*
 * This structure serves as the header for each allocated and free block.
 * It also serves as the footer for each free block but only containing size.
 */
typedef struct blockHeader {           

    int size_status;

    /*
     * Size of the block is always a multiple of 8.
     * Size is stored in all block headers and in free block footers.
     *
     * Status is stored only in headers using the two least significant bits.
     *   Bit0 => least significant bit, last bit
     *   Bit0 == 0 => free block
     *   Bit0 == 1 => allocated block
     *
     *   Bit1 => second last bit 
     *   Bit1 == 0 => previous block is free
     *   Bit1 == 1 => previous block is allocated
     * 
     * Start Heap: 
     *  The blockHeader for the first block of the heap is after skip 4 bytes.
     *  This ensures alignment requirements can be met.
     * 
     * End Mark: 
     *  The end of the available memory is indicated using a size_status of 1.
     * 
     */
} blockHeader;         

/* Global variable  
 * It must point to the first block in the heap and is set by init_heap()
 */
blockHeader *heap_start = NULL;     

/* Size of heap allocation padded to round to nearest page size.
 */
int alloc_size;
 
/* 
 * Function for allocating 'size' bytes of heap memory.
 * Argument size: requested size for the payload
 * Returns address of allocated block (payload) on success.
 * Returns NULL on failure.
 *
 * This function must:
 * - Check size - Return NULL if size < 1 
 * - Determine block size rounding up to a multiple of 8 
 *   and possibly adding padding as a result.
 *
 * - Use BEST-FIT PLACEMENT POLICY to chose a free block
 *
 * - If the BEST-FIT block that is found is exact size match
 *   - 1. Update all heap blocks as needed for any affected blocks
 *   - 2. Return the address of the allocated block payload
 *
 * - If the BEST-FIT block that is found is large enough to split 
 *   - 1. SPLIT the free block into two valid heap blocks:
 *         1. an allocated block
 *         2. a free block
 *         NOTE: both blocks must meet heap block requirements 
 *       - Update all heap block header(s) and footer(s) 
 *              as needed for any affected blocks.
 *   - 2. Return the address of the allocated block payload
 *
 *   Return if NULL unable to find and allocate block for required size
 *
 * Payload address that is returned is NOT the address of the
 *       block header.  It is the address of the start of the 
 *       available memory for the requesterr.
 *
 */
void* balloc(int size) {     
    if (size < 1) {
	    return NULL;
    }

    // block size rounding up to multiple of 8
    int blockSize = ((size + sizeof(blockHeader) + 7) / 8) * 8;

    blockHeader *current = heap_start;
    blockHeader *bestFit = NULL;

    // while not at end of heap
    while (current->size_status != 1) {
	    // remove status bits to get the size
	    int currentSize = current->size_status >> 2 << 2;
	    // if block is free and large enough
	    if (!(current->size_status & 1) && currentSize >= blockSize) {
		    // if it is the first fit or better fit
		    if (bestFit == NULL || currentSize < (bestFit->size_status >> 2 << 2)) {
			    bestFit = current;
			    // if exact size match
			    if (currentSize == blockSize) {
				    break;
			    }
		    }
	    }
	    // move to next block
	    current = (blockHeader*)((char*)current + currentSize);
    }

    // cannot find best-fit block
    if (bestFit == NULL) {
	    return NULL;
    }

    int remainder = (bestFit->size_status >> 2 << 2) - blockSize;
    // if remainder block large enough to split
    if (remainder >= sizeof(blockHeader) * 2 + 8) {
	    // update header of allocated block
	    bestFit->size_status = blockSize | (bestFit->size_status & 3) | 1;
	    // split block
	    blockHeader *newBlock = (blockHeader*)((char*)bestFit + blockSize);
	    // update header of free block
	    newBlock->size_status = remainder | 2;
	    // update footer of the free block
	    blockHeader *nextBlock = (blockHeader*)((char*)newBlock + remainder);
	    // if not at end of heap
	    if (nextBlock->size_status != 1) {
		    // set previous block allocated bit
		    nextBlock->size_status |= 2;
	    }
    }
    // if remainder block too small
    else {
	    // update header of allocated block
	    bestFit->size_status |= 1;
	    // get next block
	    blockHeader *nextBlock = (blockHeader*)((char*)bestFit + (bestFit->size_status >> 2 << 2));
	    // if not end of heap
	    if (nextBlock->size_status != 1) {
		    // set previous block allocated bit
		    nextBlock->size_status |= 2;
	    }
    }

    return bestFit + 1;
} 
 
/* 
 * Function for freeing up a previously allocated block.
 * Argument ptr: address of the block to be freed up.
 * Returns 0 on success.
 * Returns -1 on failure.
 * This function:
 * - Return -1 if ptr is NULL.
 * - Return -1 if ptr is not a multiple of 8.
 * - Return -1 if ptr is outside of the heap space.
 * - Return -1 if ptr block is already freed.
 * - Update header(s) and footer as needed.
 */                    
int bfree(void *ptr) {
    // if ptr is NULL, not mulitple of 8 or outside of heap space
    if (!ptr || (unsigned long)ptr % 8 != 0 || ptr < (void*)(heap_start + 1) || ptr >= (void*)((char*)heap_start + alloc_size)) {
	    return -1;
    }

    // get the block header
    blockHeader *block = (blockHeader*)ptr - 1;
    // if block is already freed
    if (!(block->size_status & 1)) {
	    return -1;
    }

    // mark block as free
    block->size_status &= ~1;

    // get next block
    blockHeader *nextBlock = (blockHeader*)((char*)block + (block->size_status & ~3));
    // if not at end of heap
    if (nextBlock->size_status != 1) {
	    // set previous block allocated bit
	    nextBlock->size_status &= ~2;
    }

    // freed the block
    return 0;
} 

/*
 * Function for traversing heap block list and coalescing all adjacent 
 * free blocks.
 *
 * This function is used for user-called coalescing.
 * Updated header size_status and footer size_status as needed.
 */
int coalesce() {
    blockHeader *current = heap_start;

    // while we are not at end of the heap
    while (current->size_status != 1) {
	    // if current block is free
	    if (!(current->size_status & 1)) {
		    // get next block
		    blockHeader *nextBlock = (blockHeader*)((char*)current + (current->size_status & ~3));
		    // while next block is free and not at end of heap
		    while (nextBlock->size_status != 1 && !(nextBlock->size_status & 1)) {
			    // get new next block
			    current->size_status += (nextBlock->size_status & ~3);
			    nextBlock = (blockHeader*)((char*)current + (current->size_status & ~3));
		    }

		    // if not at end of heap
		    if (nextBlock->size_status != 1) {
			    // set previous block allocated bit
			    nextBlock->size_status |= 2;
		    }
	    }
	    // move to next block
	    current = (blockHeader*)((char*)current + (current->size_status & ~3));
    }

	    // coalesced all adjacent free blocks
    return 0;
}

 
/* 
 * Function used to initialize the memory allocator.
 * Intended to be called ONLY once by a program.
 * Argument sizeOfRegion: the size of the heap space to be allocated.
 * Returns 0 on success.
 * Returns -1 on failure.
 */                    
int init_heap(int sizeOfRegion) {    
 
    static int allocated_once = 0; //prevent multiple myInit calls
 
    int   pagesize; // page size
    int   padsize;  // size of padding when heap size not a multiple of page size
    void* mmap_ptr; // pointer to memory mapped area
    int   fd;

    blockHeader* end_mark;
  
    if (0 != allocated_once) {
        fprintf(stderr, 
        "Error:mem.c: InitHeap has allocated space during a previous call\n");
        return -1;
    }

    if (sizeOfRegion <= 0) {
        fprintf(stderr, "Error:mem.c: Requested block size is not positive\n");
        return -1;
    }

    // Get the pagesize from O.S. 
    pagesize = getpagesize();

    // Calculate padsize as the padding required to round up sizeOfRegion 
    // to a multiple of pagesize
    padsize = sizeOfRegion % pagesize;
    padsize = (pagesize - padsize) % pagesize;

    alloc_size = sizeOfRegion + padsize;

    // Using mmap to allocate memory
    fd = open("/dev/zero", O_RDWR);
    if (-1 == fd) {
        fprintf(stderr, "Error:mem.c: Cannot open /dev/zero\n");
        return -1;
    }
    mmap_ptr = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (MAP_FAILED == mmap_ptr) {
        fprintf(stderr, "Error:mem.c: mmap cannot allocate space\n");
        allocated_once = 0;
        return -1;
    }
  
    allocated_once = 1;

    // for double word alignment and end mark
    alloc_size -= 8;

    // Initially there is only one big free block in the heap.
    // Skip first 4 bytes for double word alignment requirement.
    heap_start = (blockHeader*) mmap_ptr + 1;

    // Set the end mark
    end_mark = (blockHeader*)((void*)heap_start + alloc_size);
    end_mark->size_status = 1;

    // Set size in header
    heap_start->size_status = alloc_size;

    // Set p-bit as allocated in header
    // note a-bit left at 0 for free
    heap_start->size_status += 2;

    // Set the footer
    blockHeader *footer = (blockHeader*) ((void*)heap_start + alloc_size - 4);
    footer->size_status = alloc_size;
  
    return 0;
} 
                  
/* 
 * Function can be used for DEBUGGING to help you visualize your heap structure.
 * Traverses heap blocks and prints info about each block found.
 * 
 * Prints out a list of all the blocks including this information:
 * No.      : serial number of the block 
 * Status   : free/used (allocated)
 * Prev     : status of previous block free/used (allocated)
 * t_Begin  : address of the first byte in the block (where the header starts) 
 * t_End    : address of the last byte in the block 
 * t_Size   : size of the block as stored in the block header
 */                     
void disp_heap() {     
 
    int    counter;
    char   status[6];
    char   p_status[6];
    char * t_begin = NULL;
    char * t_end   = NULL;
    int    t_size;

    blockHeader *current = heap_start;
    counter = 1;

    int used_size =  0;
    int free_size =  0;
    int is_used   = -1;

    fprintf(stdout, 
	"*********************************** HEAP: Block List ****************************\n");
    fprintf(stdout, "No.\tStatus\tPrev\tt_Begin\t\tt_End\t\tt_Size\n");
    fprintf(stdout, 
	"---------------------------------------------------------------------------------\n");
  
    while (current->size_status != 1) {
        t_begin = (char*)current;
        t_size = current->size_status;
    
        if (t_size & 1) {
            // LSB = 1 => used block
            strcpy(status, "alloc");
            is_used = 1;
            t_size = t_size - 1;
        } else {
            strcpy(status, "FREE ");
            is_used = 0;
        }

        if (t_size & 2) {
            strcpy(p_status, "alloc");
            t_size = t_size - 2;
        } else {
            strcpy(p_status, "FREE ");
        }

        if (is_used) 
            used_size += t_size;
        else 
            free_size += t_size;

        t_end = t_begin + t_size - 1;
    
        fprintf(stdout, "%d\t%s\t%s\t0x%08lx\t0x%08lx\t%4i\n", counter, status, 
        p_status, (unsigned long int)t_begin, (unsigned long int)t_end, t_size);
    
        current = (blockHeader*)((char*)current + t_size);
        counter = counter + 1;
    }

    fprintf(stdout, 
	"---------------------------------------------------------------------------------\n");
    fprintf(stdout, 
	"*********************************************************************************\n");
    fprintf(stdout, "Total used size = %4d\n", used_size);
    fprintf(stdout, "Total free size = %4d\n", free_size);
    fprintf(stdout, "Total size      = %4d\n", used_size + free_size);
    fprintf(stdout, 
	"*********************************************************************************\n");
    fflush(stdout);

    return;  
} 


                                       


