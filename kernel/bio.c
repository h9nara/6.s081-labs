// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKET 13

#define BUC(dev, blockno) ((((dev)<< 27)|(blockno))%NBUCKET)

struct {
  struct spinlock evic_lock;
  struct spinlock locks[NBUCKET];
  // Not using array of pointers here to have a dummy head for each bucket
  struct buf buckets[NBUCKET];
  struct buf buf[NBUF];
} bcache;

// struct {
//   struct spinlock lock;
//   struct buf buf[NBUF];

//   // Linked list of all buffers, through prev/next.
//   // Sorted by how recently the buffer was used.
//   // head.next is most recent, head.prev is least.
//   struct buf head;
// } bcache;

void binit(void)
{
  // Initialize bufmap
  for (int i = 0; i < NBUCKET; i++)
  {
    initlock(&bcache.locks[i], "bcache_bufmap");
    bcache.buckets[i].next = 0;
  }

  // Initialize buffers
  for (int i = 0; i < NBUF; i++)
  {
    struct buf *b = &bcache.buf[i];
    initsleeplock(&b->lock, "buffer");
    b->last_used = 0;
    b->refcnt = 0;
    // put all the buffers into bufmap[0]
    b->next = bcache.buckets[0].next;
    bcache.buckets[0].next = b;
  }

  initlock(&bcache.evic_lock, "bcache_eviction");
}

// void binit(void)
// {
//   for (int i = 0; i < NBUCKET; i++)
//   {
//     initlock(&bcache.locks[i], "bcache_bucket_locks");
//     bcache.buckets[i].next = 0;
//   }

//   // Initialize the buffers.
//   for (int i = 0; i < NBUF; i++)
//   {
//     struct buf *b = &bcache.buf[i];
//     initsleeplock(&b->lock, "buffer");
//     b->refcnt = 0;
//     b->last_used = 0;
//     // Add all the empty buffers to bucket 0.
//     b->next = bcache.buckets[0].next;
//     bcache.buckets[0].next = b;
//   }
//   initlock(&bcache.evic_lock, "bcache_eviction");
//   // // Create linked list of buffers
//   // bcache.head.prev = &bcache.head;
//   // bcache.head.next = &bcache.head;
//   // for(b = bcache.buf; b < bcache.buf+NBUF; b++){
//   //   b->next = bcache.head.next;
//   //   b->prev = &bcache.head;
//   //   initsleeplock(&b->lock, "buffer");
//   //   bcache.head.next->prev = b;
//   //   bcache.head.next = b;
//   // }
// }

static struct buf *
bget(uint dev, uint blockno)
{
  struct buf *b;

  uint key = BUC(dev, blockno);

  acquire(&bcache.locks[key]);

  // Is the block already cached?
  for (b = bcache.buckets[key].next; b; b = b->next)
  {
    if (b->dev == dev && b->blockno == blockno)
    {
      b->refcnt++;
      release(&bcache.locks[key]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.

  // to get a suitable block to reuse, we need to search for one in all the buckets,
  // which means acquiring their bucket locks.
  // but it's not safe to try to acquire every single bucket lock while holding one.
  // it can easily lead to circular wait, which produces deadlock.

  release(&bcache.locks[key]);
  // we need to release our bucket lock so that iterating through all the buckets won't
  // lead to circular wait and deadlock. however, as a side effect of releasing our bucket
  // lock, other cpus might request the same blockno at the same time and the cache buf for
  // blockno might be created multiple times in the worst case. since multiple concurrent
  // bget requests might pass the "Is the block already cached?" test and start the
  // eviction & reuse process multiple times for the same blockno.
  //
  // so, after acquiring evic_lock, we check "whether cache for blockno is present"
  // once more, to be sure that we don't create duplicate cache bufs.
  acquire(&bcache.evic_lock);

  // Check again, is the block already cached?
  // no other eviction & reuse will happen while we are holding evic_lock,
  // which means no link list structure of any bucket can change.
  // so it's ok here to iterate through `bcache.buf[key]` without holding
  // it's cooresponding bucket lock, since we are holding a much stronger evic_lock.
  for (b = bcache.buckets[key].next; b; b = b->next)
  {
    if (b->dev == dev && b->blockno == blockno)
    {
      acquire(&bcache.locks[key]); // must do, for `refcnt++`
      b->refcnt++;
      release(&bcache.locks[key]);
      release(&bcache.evic_lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Still not cached.
  // we are now only holding eviction lock, none of the bucket locks are held by us.
  // so it's now safe to acquire any bucket's lock without risking circular wait and deadlock.

  // find the one least-recently-used buf among all buckets.
  // finish with it's corresponding bucket's lock held.
  struct buf *before_least = 0;
  uint holding_bucket = -1;
  for (int i = 0; i < NBUCKET; i++)
  {
    // before acquiring, we are either holding nothing, or only holding locks of
    // buckets that are *on the left side* of the current bucket
    // so no circular wait can ever happen here. (safe from deadlock)
    acquire(&bcache.locks[i]);
    int newfound = 0; // new least-recently-used buf found in this bucket
    for (b = &bcache.buckets[i]; b->next; b = b->next)
    {
      if (b->next->refcnt == 0 && (!before_least || b->next->last_used < before_least->next->last_used))
      {
        before_least = b;
        newfound = 1;
      }
    }
    if (!newfound)
    {
      release(&bcache.locks[i]);
    }
    else
    {
      if (holding_bucket != -1)
        release(&bcache.locks[holding_bucket]);
      holding_bucket = i;
      // keep holding this bucket's lock....
    }
  }
  if (!before_least)
  {
    panic("bget: no buffers");
  }
  b = before_least->next;

  if (holding_bucket != key)
  {
    // remove the buf from it's original bucket
    before_least->next = b->next;
    release(&bcache.locks[holding_bucket]);
    // rehash and add it to the target bucket
    acquire(&bcache.locks[key]);
    b->next = bcache.buckets[key].next;
    bcache.buckets[key].next = b;
  }

  b->dev = dev;
  b->blockno = blockno;
  b->refcnt = 1;
  b->valid = 0;
  release(&bcache.locks[key]);
  release(&bcache.evic_lock);
  acquiresleep(&b->lock);
  return b;
}

// -----------------------------------------------------------------------------

// static struct buf *bget(uint dev, uint blockno)
// {
//   // acquire(&bcache.evic_lock);
//   uint key = BUC(dev, blockno);

//   struct buf *b;
//   acquire(&bcache.locks[key]);
//   // Is the block already cached?
//   for (b = bcache.buckets[key].next; b; b = b->next)
//   {
//     // found it in the cache
//     if (b->dev == dev && b->blockno == blockno)
//     {
//       (b->refcnt)++;
//       release(&bcache.locks[key]);
//       // release(&bcache.evic_lock);
//       acquiresleep(&b->lock);
//       return b;
//     }
//   }
//   // Must release the lock for the key bucket to avoid circular deadlock.
//   release(&bcache.locks[key]);
//   // Acquire the eviction lock so that there won't be multiple CPUs going
//   // thourgh the look-up and evict process for the same block which leads to
//   // multiple cache for one single block.
//   acquire(&bcache.evic_lock);
//   // Check if the block is already cached again immediately after acquiring the
//   // eviction lock.
//   for (b = bcache.buckets[key].next; b; b = b->next)
//   {
//     // found it in the cache
//     if (b->dev == dev && b->blockno == blockno)
//     {
//       acquire(&bcache.locks[key]);
//       (b->refcnt)++;
//       release(&bcache.locks[key]);
//       release(&bcache.evic_lock);
//       acquiresleep(&b->lock);
//       return b;
//     }
//   }

//   struct buf *lru_prev = 0;
//   uint bucket_held = -1;
//   for (int i = 0; i < NBUCKET; i++)
//   {
//     // hold the current bucket lock.
//     acquire(&bcache.locks[i]);
//     int updated = 0;
//     for (struct buf *b = &bcache.buckets[i]; b->next; b = b->next)
//     {
//       // Only evict a block which ref count is 0.
//       if (b->next->refcnt > 0)
//       {
//         continue;
//       }
//       if (!lru_prev || b->next->last_used < lru_prev->next->last_used)
//       {
//         lru_prev = b;
//         updated = 1;
//       }
//     }
//     // Not updated in the current bucket, release the current bucket lock.
//     if (!updated)
//     {
//       release(&bcache.locks[i]);
//       continue;
//     }
//     if (bucket_held != -1)
//     {
//       release(&bcache.locks[bucket_held]);
//     }
//     bucket_held = i;
//   }
//   if (!lru_prev)
//   {
//     panic("bget: no buffers");
//   }

//   // Evict the LRU buffer.
//   // bcache.locks[bucket_held] held here.
//   // b points to the buffer to be evicted.
//   b = lru_prev->next;
//   // Remove the LRU buffer from its bucket and add it to the key bucket.
//   if (bucket_held != key)
//   {
//     lru_prev->next = b->next;
//     release(&bcache.locks[bucket_held]);
//     acquire(&bcache.locks[key]);
//     b->next = bcache.buckets[key].next;
//     bcache.buckets[key].next = b;
//   }
//   b->dev = dev;
//   b->blockno = blockno;
//   b->valid = 0;
//   b->refcnt = 1;
//   release(&bcache.locks[key]);
//   release(&bcache.evic_lock);
//   acquiresleep(&b->lock);
//   return b;
// }

// -----------------------------------------------------------------------------

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
// static struct buf*
// bget(uint dev, uint blockno)
// {
//   struct buf *b;

//   acquire(&bcache.lock);

//   // Is the block already cached?
//   for(b = bcache.head.next; b != &bcache.head; b = b->next){
//     if(b->dev == dev && b->blockno == blockno){
//       b->refcnt++;
//       release(&bcache.lock);
//       acquiresleep(&b->lock);
//       return b;
//     }
//   }

//   // Not cached.
//   // Recycle the least recently used (LRU) unused buffer.
//   for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
//     if(b->refcnt == 0) {
//       b->dev = dev;
//       b->blockno = blockno;
//       b->valid = 0;
//       b->refcnt = 1;
//       release(&bcache.lock);
//       acquiresleep(&b->lock);
//       return b;
//     }
//   }
//   panic("bget: no buffers");
// }

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

void brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);
  uint key = BUC(b->dev, b->blockno);
  acquire(&bcache.locks[key]);
  b->refcnt--;
  if (b->refcnt == 0)
    b->last_used = ticks;
  release(&bcache.locks[key]);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
// void
// brelse(struct buf *b)
// {
//   if(!holdingsleep(&b->lock))
//     panic("brelse");

//   releasesleep(&b->lock);

//   acquire(&bcache.lock);
//   b->refcnt--;
//   if (b->refcnt == 0) {
//     // no one is waiting for it.
//     b->next->prev = b->prev;
//     b->prev->next = b->next;
//     b->next = bcache.head.next;
//     b->prev = &bcache.head;
//     bcache.head.next->prev = b;
//     bcache.head.next = b;
//   }

//   release(&bcache.lock);
// }

void
bpin(struct buf *b) {
  // acquire(&bcache.lock);
  uint key = BUC(b->dev, b->blockno);
  acquire(&bcache.locks[key]);
  b->refcnt++;
  release(&bcache.locks[key]);
  // release(&bcache.lock);
}

void
bunpin(struct buf *b) {
  // acquire(&bcache.lock);
  uint key = BUC(b->dev, b->blockno);
  acquire(&bcache.locks[key]);
  b->refcnt--;
  release(&bcache.locks[key]);
  // release(&bcache.lock);
}


