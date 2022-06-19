struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  uint last_used;

  // LRU cache is implemented using timestamps now.
  // struct buf *prev; // LRU cache list
  struct buf *next;
  uchar data[BSIZE];
};

