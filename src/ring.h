#ifndef _ring_h_
#define _ring_h_


typedef enum {
    BLOCK,
    LIGHT,
    KEY,
    COMMIT,
    EXIT,
    BLOCK_DAMAGE,
    BLOCK_DAMAGE_TRIM,
} RingEntryType;


typedef struct {
    RingEntryType type;
    int p;
    int q;
    int x;
    int y;
    int z;
    int w;
    int key;
} RingEntry;


typedef struct {
    unsigned int capacity;
    unsigned int start;
    unsigned int end;
    RingEntry *data;
} Ring;


void ring_alloc(
        Ring *ring,
        int capacity);

int ring_empty(
        Ring *ring);

void ring_free(
        Ring *ring);

int ring_full(
        Ring *ring);

int ring_get(
        Ring *ring,
        RingEntry *entry);

void ring_grow(
        Ring *ring);

void ring_put(
        Ring *ring,
        RingEntry *entry);

void ring_put_block(
        Ring *ring,
        int p,
        int q,
        int x,
        int y,
        int z,
        int w);

void ring_put_block_damage(
        Ring *ring,
        int p,
        int q,
        int x,
        int y,
        int z,
        int damage);

void ring_put_block_damage_trim(
        Ring *ring,
        int p,
        int q);

void ring_put_commit(
        Ring *ring);

void ring_put_exit(
        Ring *ring);

void ring_put_key(
        Ring *ring,
        int p,
        int q,
        int key);

void ring_put_light(
        Ring *ring,
        int p,
        int q,
        int x,
        int y,
        int z,
        int w);

int ring_size(
        Ring *ring);


#endif
