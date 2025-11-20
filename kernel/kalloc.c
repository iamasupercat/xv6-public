// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

// pa4: struct for page control
struct page pages[PHYSTOP/PGSIZE];
struct page *page_lru_head;
int num_free_pages;
int num_lru_pages;

// Swap bitmap: track which swap slots are in use
// Use 1 physical page for the bitmap
#define SWAP_PAGES (SWAPMAX / (PGSIZE / BSIZE))
uint8 *swap_bitmap;  // Points to a physical page allocated in kinit()
struct spinlock swap_lock;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&swap_lock, "swap");
  page_lru_head = 0;
  num_lru_pages = 0;
  
  // Allocate physical page(s) for swap bitmap
  // Calculate required bytes: (SWAP_PAGES + 7) / 8
  int nbytes = (SWAP_PAGES + 7) / 8;
  int npages = (nbytes + PGSIZE - 1) / PGSIZE;
  
  // Allocate first page
  swap_bitmap = (uint8*)kalloc();
  if(swap_bitmap == 0)
    panic("kinit: failed to allocate swap bitmap");
  memset(swap_bitmap, 0, PGSIZE);
  
  // Note: If npages > 1, we would need to allocate additional pages
  // and manage them. Currently 1 page is sufficient (875 bytes needed).
  
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Helper function to get page struct from physical address
struct page*
pa2page(uint64 pa)
{
  if(pa < (uint64)end || pa >= PHYSTOP)
    return 0;
  return &pages[(pa - (uint64)end) / PGSIZE];
}

// Add page to LRU list (circular doubly linked list)
void
lru_add(struct page *pg)
{
  acquire(&kmem.lock);
  
  if(page_lru_head == 0) {
    // First page in list
    page_lru_head = pg;
    pg->next = pg;
    pg->prev = pg;
  } else {
    // Add to tail (before head)
    pg->next = page_lru_head;
    pg->prev = page_lru_head->prev;
    page_lru_head->prev->next = pg;
    page_lru_head->prev = pg;
  }
  num_lru_pages++;
  
  release(&kmem.lock);
}

// Remove page from LRU list
void
lru_remove(struct page *pg)
{
  acquire(&kmem.lock);
  
  if(pg->next == pg) {
    // Only one page in list
    page_lru_head = 0;
  } else {
    pg->prev->next = pg->next;
    pg->next->prev = pg->prev;
    if(page_lru_head == pg)
      page_lru_head = pg->next;
  }
  pg->next = 0;
  pg->prev = 0;
  num_lru_pages--;
  
  release(&kmem.lock);
}

// Move page to tail of LRU list (assumes lock is already held)
static void
lru_move_to_tail_locked(struct page *pg)
{
  if(page_lru_head == 0 || pg->next == 0)
    return;
  
  // Remove from current position
  pg->prev->next = pg->next;
  pg->next->prev = pg->prev;
  if(page_lru_head == pg)
    page_lru_head = pg->next;
  
  // Add to tail
  pg->next = page_lru_head;
  pg->prev = page_lru_head->prev;
  page_lru_head->prev->next = pg;
  page_lru_head->prev = pg;
}

// Find free swap slot
static int
swap_alloc(void)
{
  int i, j;
  acquire(&swap_lock);
  
  for(i = 0; i < SWAP_PAGES; i++) {
    j = i / 8;
    if((swap_bitmap[j] & (1 << (i % 8))) == 0) {
      swap_bitmap[j] |= (1 << (i % 8));
      release(&swap_lock);
      return i;
    }
  }
  
  release(&swap_lock);
  return -1;
}

// Free swap slot
void
swap_free(int blkno)
{
  int j;
  if(blkno < 0 || blkno >= SWAP_PAGES)
    return;
  
  acquire(&swap_lock);
  j = blkno / 8;
  swap_bitmap[j] &= ~(1 << (blkno % 8));
  release(&swap_lock);
}

// Swap-out: evict a page to disk using Clock algorithm
static int
swapout(void)
{
  struct page *victim = 0;
  pte_t *pte;
  uint64 pa, flags;
  int swap_blkno;
  
  if(page_lru_head == 0)
    return -1;
  
  acquire(&kmem.lock);
  
  // Clock algorithm: find page with PTE_A == 0
  struct page *curr = page_lru_head;
  int rounds = 0;
  
  do {
    if(curr->pagetable == 0 || curr->vaddr == 0) {
      curr = curr->next;
      rounds++;
      if(rounds > num_lru_pages)
        break;
      continue;
    }
    
    pte = walk(curr->pagetable, (uint64)curr->vaddr, 0);
    if(pte == 0 || (*pte & PTE_V) == 0) {
      curr = curr->next;
      rounds++;
      if(rounds > num_lru_pages)
        break;
      continue;
    }
    
    // Check PTE_A bit
    if((*pte & PTE_A) == 0) {
      // Found victim
      victim = curr;
      break;
    } else {
      // Clear PTE_A and move to tail
      *pte &= ~PTE_A;
      lru_move_to_tail_locked(curr);
    }
    
    curr = curr->next;
    rounds++;
  } while(curr != page_lru_head && rounds <= num_lru_pages);
  
  release(&kmem.lock);
  
  if(victim == 0)
    return -1;
  
  // Allocate swap slot
  swap_blkno = swap_alloc();
  if(swap_blkno < 0)
    return -1;
  
  // Get PTE again
  pte = walk(victim->pagetable, (uint64)victim->vaddr, 0);
  if(pte == 0 || (*pte & PTE_V) == 0) {
    swap_free(swap_blkno);
    return -1;
  }
  
  pa = PTE2PA(*pte);
  flags = PTE_FLAGS(*pte);
  
  // Write to swap
  swapwrite(pa, swap_blkno);
  
  // Update PTE: clear PTE_V, store swap offset in PPN
  *pte = (swap_blkno << 10) | (flags & ~PTE_V);
  
  // Remove from LRU list
  lru_remove(victim);
  
  // Free physical page
  kfree((void*)pa);
  
  return 0;
}

// Swap-in: load a page from disk
int
swapin(pagetable_t pagetable, uint64 va, pte_t *pte)
{
  uint64 swap_offset;
  uint64 pa;
  int swap_blkno;
  uint flags;
  struct page *pg;
  
  // Extract swap offset from PTE (stored in PPN field)
  swap_offset = (*pte) >> 10;
  swap_blkno = (int)swap_offset;
  
  if(swap_blkno < 0 || swap_blkno >= SWAP_PAGES)
    return -1;
  
  // Allocate new physical page
  pa = (uint64)kalloc();
  if(pa == 0)
    return -1;
  
  // Read from swap
  swapread(pa, swap_blkno);
  
  // Get flags (preserve original flags except PTE_V)
  flags = PTE_FLAGS(*pte) | PTE_V | PTE_A;
  
  // Update PTE
  *pte = PA2PTE(pa) | flags;
  
  // Add to LRU list
  pg = pa2page(pa);
  if(pg) {
    pg->pagetable = pagetable;
    pg->vaddr = (char*)va;
    lru_add(pg);
  }
  
  // Free swap slot
  swap_free(swap_blkno);
  
  return 0;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
// pa4: kalloc function with swap support
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  // If no free page, try swap-out
  if(r == 0) {
    if(swapout() < 0) {
      printf("kalloc: out of memory\n");
      return 0;
    }
    // Try again after swap-out
    acquire(&kmem.lock);
    r = kmem.freelist;
    if(r)
      kmem.freelist = r->next;
    release(&kmem.lock);
  }

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}