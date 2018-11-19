/**
 * Implementace My MALloc
 * Demonstracni priklad pro 1. ukol IPS/2018
 * Ales Smrcka
 */

#include "mmal.h"
#include <sys/mman.h> // mmap
#include <stdbool.h> // bool
#include <string.h> //memcpy
#include <assert.h> // assert

#ifdef NDEBUG
/**
 * The structure header encapsulates data of a single memory block.
 *   ---+------+----------------------------+---
 *      |Header|DDD not_free DDDDD...free...|
 *   ---+------+-----------------+----------+---
 *             |-- Header.asize -|
 *             |-- Header.size -------------|
 */
typedef struct header Header;
struct header {

    /**
     * Pointer to the next header. Cyclic list. If there is no other block,
     * points to itself.
     */
    Header *next;

    /// size of the block
    size_t size;

    /**
     * Size of block in bytes allocated for program. asize=0 means the block 
     * is not used by a program.
     */
    size_t asize;
};

/**
 * The arena structure.
 *   /--- arena metadata
 *   |     /---- header of the first block
 *   v     v
 *   +-----+------+-----------------------------+
 *   |Arena|Header|.............................|
 *   +-----+------+-----------------------------+
 *
 *   |--------------- Arena.size ---------------|
 */
typedef struct arena Arena;
struct arena {

    /**
     * Pointer to the next arena. Single-linked list.
     */
    Arena *next;

    /// Arena size.
    size_t size;
};

#define PAGE_SIZE (128*1024)

#endif // NDEBUG

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#endif

Arena *first_arena = NULL;

/**
 * Return size alligned to PAGE_SIZE
 */
static
size_t allign_page(size_t size)
{
    size = ((size + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;
    return size;
}

/**
 * Allocate a new arena using mmap.
 * @param req_size requested size in bytes. Should be alligned to PAGE_SIZE.
 * @return pointer to a new arena, if successfull. NULL if error.
 * @pre req_size > sizeof(Arena) + sizeof(Header)
 */

/**
 *   +-----+------------------------------------+
 *   |Arena|....................................|
 *   +-----+------------------------------------+
 *
 *   |--------------- Arena.size ---------------|
 */
static
Arena *arena_alloc(size_t req_size)
{
    assert(req_size > sizeof(Arena) + sizeof(Header));
    static Arena *temp;
    temp = mmap(NULL, req_size, 
            PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (temp == (void*)-1){
        return NULL;
    }
    temp->size = req_size;
    return temp;
}

/**
 * Appends a new arena to the end of the arena list.
 * @param a     already allocated arena
 */
static
void arena_append(Arena *a)
{
    static Arena *end = NULL;
    if(first_arena == NULL)
        first_arena = a;
    else
        end->next = a;
    end = a;
    
}

/**
 * Header structure constructor (alone, not used block).
 * @param hdr       pointer to block metadata.
 * @param size      size of free block
 * @pre size > 0
 */
/**
 *   +-----+------+------------------------+----+
 *   | ... |Header|........................| ...|
 *   +-----+------+------------------------+----+
 *
 *                |-- Header.size ---------|
 */
static
void hdr_ctor(Header *hdr, size_t size)
{
    assert(size > 0);
    static Header *temp;
    hdr->size = size;
    hdr->asize = 0;
    if(temp != NULL)
    {
        hdr->next = temp->next;
        temp->next = hdr;
    }
    else
    {
        hdr->next = hdr;
    }
    temp = hdr;
}

/**
 * Checks if the given free block should be split in two separate blocks.
 * @param hdr       header of the free block
 * @param size      requested size of data
 * @return true if the block should be split
 * @pre hdr->asize == 0
 * @pre size > 0
 */
static
bool hdr_should_split(Header *hdr, size_t size)
{
    assert(hdr->asize == 0);
    assert(size > 0);
    // FIXME
    if(hdr->size >= sizeof(Header) + size + 1)
        return true;
    return false;
}

/**
 * Splits one block in two.
 * @param hdr       pointer to header of the big block
 * @param req_size  requested size of data in the (left) block.
 * @return pointer to the new (right) block header.
 * @pre   (hdr->size >= req_size + 2*sizeof(Header))
 */
/**
 * Before:        |---- hdr->size ---------|
 *
 *    -----+------+------------------------+----
 *         |Header|........................|
 *    -----+------+------------------------+----
 *            \----hdr->next---------------^
 */
/**
 * After:         |- req_size -|
 *
 *    -----+------+------------+------+----+----
 *     ... |Header|............|Header|....|
 *    -----+------+------------+------+----+----
 *             \---next--------^  \--next--^
 */
static
Header *hdr_split(Header *hdr, size_t req_size)
{
    assert((hdr->size >= req_size + 2*sizeof(Header)));
    
    Header *temp = (Header *) ((char *) hdr + sizeof(Header) + req_size);
    temp->next = hdr->next;
    hdr->next = temp;
    temp->asize = 0;
    temp->size = hdr->size - req_size - sizeof(Header);
    hdr->size = req_size;

    return temp;
}

/**
 * Detect if two adjacent blocks could be merged.
 * @param left      left block
 * @param right     right block
 * @return true if two block are free and adjacent in the same arena.
 * @pre left->next == right
 * @pre left != right
 */
static
bool hdr_can_merge(Header *left, Header *right)
{
    assert(left->next == right);
    assert(left != right);
    if((Header *)(((char *)left) + left->size + sizeof(Header)) == right && 
            left->asize == 0 && right->asize == 0)
        return true;
    return false;
}

/**
 * Merge two adjacent free blocks.
 * @param left      left block
 * @param right     right block
 * @pre left->next == right
 * @pre left != right
 */
static
void hdr_merge(Header *left, Header *right)
{
    assert(left->next == right);
    assert(left != right);
    left->size += right->size + sizeof(Header);
    left->next = right->next;
    right = NULL;
}

/**
 * Finds the first free block that fits to the requested size.
 * @param size      requested size
 * @return pointer to the header of the block or NULL if no block is available.
 * @pre size > 0
 */
static
Header *first_fit(size_t size)
{
    assert(size > 0);
    Header *header = (Header *) (first_arena + 1);
    Header *first = header;
    do
    {
        header = header->next;
        if(header->asize == 0 && header->size >= size)
            return header;
    }while(header != first);
    return NULL;
}

/**
 * Search the header which is the predecessor to the hdr. Note that if 
 * @param hdr       successor of the search header
 * @return pointer to predecessor, hdr if there is just one header.
 * @pre first_arena != NULL
 * @post predecessor->next == hdr
 */
static
Header *hdr_get_prev(Header *hdr)
{
    assert(first_arena != NULL);
    Header *temp = hdr;
    while(temp->next != hdr){
        temp = temp->next;
    }
    return temp;
}

/**
 * Allocate memory. Use first-fit search of available block.
 * @param size      requested size for program
 * @return pointer to allocated data or NULL if error or size = 0.
 */
void *mmalloc(size_t size)
{
    if(first_arena == NULL)
    {
        Arena *ar = arena_alloc(allign_page(size + sizeof(Header) + sizeof(Arena)));
        arena_append(ar);
        Header *new_header = (Header *) (ar + 1);
        hdr_ctor(new_header, 
                ar->size - sizeof(Header) - sizeof(Arena));
    }
    Header *block = first_fit(size);
    if(block)
    {
        if(hdr_should_split(block, size))
            hdr_split(block, size);
        block->asize = size;
        return (void *) (block + 1);
    }
    else
    {
        Arena *new = arena_alloc(allign_page(size + sizeof(Header) + sizeof(Arena)));
        arena_append(new);
        Header *new_header = (Header *) (new + 1);
        hdr_ctor(new_header, 
                new->size - sizeof(Header) - sizeof(Arena));
        if(hdr_should_split(new_header, size))
            hdr_split(new_header, size);
        new_header->asize = size;
        return (void *)(new_header + 1);
    }

    return NULL;
}

/**
 * Free memory block.
 * @param ptr       pointer to previously allocated data
 * @pre ptr != NULL
 */
void mfree(void *ptr)
{
    assert(ptr != NULL);
    Header *header = ((Header *)ptr - 1);
    header->asize = 0;
    if(header < header->next && hdr_can_merge(header, header->next))
        hdr_merge(header, header->next);
    Header *prev = hdr_get_prev(header);
    if(prev < header && hdr_can_merge(prev, header))
        hdr_merge(prev, header);

}

/**
 * Reallocate previously allocated block.
 * @param ptr       pointer to previously allocated data
 * @param size      a new requested size. Size can be greater, equal, or less
 * then size of previously allocated block.
 * @return pointer to reallocated space or NULL if size equals to 0.
 */
void *mrealloc(void *ptr, size_t size)
{
    Header *header = ((Header *)ptr) - 1;
    Header *next_header = header->next;
    if(header->size == size)
        return ptr;
    else if(header->size > size)
    {
        header->asize = size;
        return ptr;
    }
    else if(next_header->asize == 0 && 
            (((char *)header) + header->size + sizeof(Header)) == next_header &&
            next_header->size + header->size + sizeof(Header) > size)
    {
        header->next = next_header->next;
        header->size += next_header->size + sizeof(Header);
        header->asize = size;
        return ptr;
    }
    else
    {
        void *new_ptr = mmalloc(size);
        if(new_ptr != NULL)
        {
            memcpy(new_ptr, ptr, header->asize);
            mfree(ptr);
        }
        return new_ptr;
    }
    return NULL;
}
