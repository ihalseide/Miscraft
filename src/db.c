#include "db.h"
#include "ring.h"
#include "sqlite3.h"
#include "tinycthread.h"
#include <stdio.h>
#include <string.h>

// Database code to save and load worlds.
// Only player-made changes from the original generated world are saved in the db.

static int db_enabled = 0;

static sqlite3 *db;
static sqlite3_stmt *insert_block_stmt;
static sqlite3_stmt *insert_light_stmt;
static sqlite3_stmt *insert_sign_stmt;
static sqlite3_stmt *delete_sign_stmt;
static sqlite3_stmt *delete_signs_stmt;
static sqlite3_stmt *load_blocks_stmt;
static sqlite3_stmt *load_lights_stmt;
static sqlite3_stmt *load_signs_stmt;
static sqlite3_stmt *get_key_stmt;
static sqlite3_stmt *set_key_stmt;
static sqlite3_stmt *load_block_damage_stmt;
static sqlite3_stmt *insert_block_damage_stmt;
static sqlite3_stmt *trim_block_damage_stmt;

static Ring ring;
static thrd_t thrd;
static mtx_t mtx;
static cnd_t cnd;
static mtx_t load_mtx;


// Enable the database
// (Used because the variable db_enabled is private to this file).
// Arguments: none
// Returns: none
void db_enable() { db_enabled = 1; }


// Disable the database
// (Used because the variable db_enabled is private to this file).
// Arguments: none
// Returns: none
void db_disable() { db_enabled = 0; }


// Get whether the database is enabled or not.
// (Used because the variable db_enabled is private to this file).
// Arguments: none
// Returns:
// - whether the database is enabled
int get_db_enabled() { return db_enabled; }


// Just used to print an error message within db_init
static int bail(int rc) {
    fprintf(stderr, "sqlite database error: %s\n", sqlite3_errmsg(db));
    return rc;
}


// Initialize a database stored in the a file with the given path (file may or may not exist).
// If the file exists, this creates each database table only if it does not already exist.
// Arguments:
// - path: path to database file to use or create if non-existent
// Returns:
// - non-zero if there was a database error
int db_init(char *path) {
    if (!db_enabled) { return 0; }
    static const char *create_query =
        "attach database 'auth.db' as auth;"
        "create table if not exists auth.identity_token ("
        "   username text not null,"
        "   token text not null,"
        "   selected int not null"
        ");"
        "create unique index if not exists auth.identity_token_username_idx"
        "   on identity_token (username);"
        "create table if not exists state ("
        "   x float not null,"
        "   y float not null,"
        "   z float not null,"
        "   rx float not null,"
        "   ry float not null,"
        "   flying int not null"
        ");"
        "create table if not exists block ("
        "    p int not null,"
        "    q int not null,"
        "    x int not null,"
        "    y int not null,"
        "    z int not null,"
        "    w int not null"
        ");"
        "create table if not exists light ("
        "    p int not null,"
        "    q int not null,"
        "    x int not null,"
        "    y int not null,"
        "    z int not null,"
        "    w int not null"
        ");"
        "create table if not exists key ("
        "    p int not null,"
        "    q int not null,"
        "    key int not null"
        ");"
        "create table if not exists sign ("
        "    p int not null,"
        "    q int not null,"
        "    x int not null,"
        "    y int not null,"
        "    z int not null,"
        "    face int not null,"
        "    text text not null"
        ");"
        "create table if not exists block_damage ("
        "    p int not null,"
        "    q int not null,"
        "    x int not null,"
        "    y int not null,"
        "    z int not null,"
        "    w int not null"
        ");"
        "create unique index if not exists block_pqxyz_idx on block (p, q, x, y, z);"
        "create unique index if not exists light_pqxyz_idx on light (p, q, x, y, z);"
        "create unique index if not exists key_pq_idx on key (p, q);"
        "create unique index if not exists sign_xyzface_idx on sign (x, y, z, face);"
        "create index if not exists sign_pq_idx on sign (p, q);"
        "create unique index if not exists damage_pqxyz_idx on block_damage (p, q, x, y, z);";
    static const char *insert_block_query =
        "insert or replace into block (p, q, x, y, z, w) "
        "values (?, ?, ?, ?, ?, ?);";
    static const char *insert_light_query =
        "insert or replace into light (p, q, x, y, z, w) "
        "values (?, ?, ?, ?, ?, ?);";
    static const char *insert_sign_query =
        "insert or replace into sign (p, q, x, y, z, face, text) "
        "values (?, ?, ?, ?, ?, ?, ?);";
    static const char *delete_sign_query =
        "delete from sign where x = ? and y = ? and z = ? and face = ?;";
    static const char *delete_signs_query =
        "delete from sign where x = ? and y = ? and z = ?;";
    static const char *load_blocks_query =
        "select x, y, z, w from block where p = ? and q = ?;";
    static const char *load_lights_query =
        "select x, y, z, w from light where p = ? and q = ?;";
    static const char *load_signs_query =
        "select x, y, z, face, text from sign where p = ? and q = ?;";
    static const char *get_key_query =
        "select key from key where p = ? and q = ?;";
    static const char *set_key_query =
        "insert or replace into key (p, q, key) "
        "values (?, ?, ?);";
    static const char *load_block_damage_query =
        "select x, y, z, w from block_damage where p = ? and q = ?;";
    static const char *insert_block_damage_query =
        "insert or replace into block_damage (p, q, x, y, z, w) "
        "values (?, ?, ?, ?, ?, ?);";
    static const char *trim_block_damage_query =
        "delete from block_damage where w=0 and p=? and q=?;";

    int rc;

    rc = sqlite3_open(path, &db);
    if (rc) { return bail(rc); }

    rc = sqlite3_exec(db, create_query, NULL, NULL, NULL);
    if (rc) { return bail(rc); }

    rc = sqlite3_prepare_v2(db, insert_block_query, -1, &insert_block_stmt, NULL);
    if (rc) { return bail(rc); }

    rc = sqlite3_prepare_v2(db, insert_light_query, -1, &insert_light_stmt, NULL);
    if (rc) { return bail(rc); }

    rc = sqlite3_prepare_v2(db, insert_sign_query, -1, &insert_sign_stmt, NULL);
    if (rc) { return bail(rc); }

    rc = sqlite3_prepare_v2(db, delete_sign_query, -1, &delete_sign_stmt, NULL);
    if (rc) { return bail(rc); }

    rc = sqlite3_prepare_v2(db, delete_signs_query, -1, &delete_signs_stmt, NULL);
    if (rc) { return bail(rc); }

    rc = sqlite3_prepare_v2(db, load_blocks_query, -1, &load_blocks_stmt, NULL);
    if (rc) { return bail(rc); }

    rc = sqlite3_prepare_v2(db, load_lights_query, -1, &load_lights_stmt, NULL);
    if (rc) { return bail(rc); }

    rc = sqlite3_prepare_v2(db, load_signs_query, -1, &load_signs_stmt, NULL);
    if (rc) { return bail(rc); }

    rc = sqlite3_prepare_v2(db, get_key_query, -1, &get_key_stmt, NULL);
    if (rc) { return bail(rc); }

    rc = sqlite3_prepare_v2(db, set_key_query, -1, &set_key_stmt, NULL);
    if (rc) { return bail(rc); }

    rc = sqlite3_prepare_v2(db, load_block_damage_query, -1, &load_block_damage_stmt, NULL);
    if (rc) { return bail(rc); }

    rc = sqlite3_prepare_v2(db, insert_block_damage_query, -1, &insert_block_damage_stmt, NULL);
    if (rc) { return bail(rc); }

    rc = sqlite3_prepare_v2(db, trim_block_damage_query, -1, &trim_block_damage_stmt, NULL);
    if (rc) { return bail(rc); }

    sqlite3_exec(db, "begin;", NULL, NULL, NULL);
    db_worker_start(NULL);
    return 0;
}


// Close the database and save pending commits
// Arguments: none
// Returns: none
void db_close() {
    if (!db_enabled) { return; }
    db_worker_stop();
    sqlite3_exec(db, "commit;", NULL, NULL, NULL);
    sqlite3_finalize(insert_block_stmt);
    sqlite3_finalize(insert_light_stmt);
    sqlite3_finalize(insert_sign_stmt);
    sqlite3_finalize(delete_sign_stmt);
    sqlite3_finalize(delete_signs_stmt);
    sqlite3_finalize(load_blocks_stmt);
    sqlite3_finalize(load_lights_stmt);
    sqlite3_finalize(load_signs_stmt);
    sqlite3_finalize(get_key_stmt);
    sqlite3_finalize(set_key_stmt);
    sqlite3_finalize(insert_block_damage_stmt);
    sqlite3_finalize(load_block_damage_stmt);
    sqlite3_finalize(trim_block_damage_stmt);
    sqlite3_close(db);
}


// Let one of the workers do the database commit.
// Arguments: none
// Returns: none
void db_commit() {
    if (!db_enabled) { return; }
    mtx_lock(&mtx);
    ring_put_commit(&ring);
    cnd_signal(&cnd);
    mtx_unlock(&mtx);
}


// Actually do a database commit.
// Arguments: none
// Returns: none
static void _db_commit() {
    sqlite3_exec(db, "commit; begin;", NULL, NULL, NULL);
}


void db_auth_set(char *username, char *identity_token) {
    if (!db_enabled) { return; }
    static const char *query =
        "insert or replace into auth.identity_token "
        "(username, token, selected) values (?, ?, ?);";
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, query, -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, username, -1, NULL);
    sqlite3_bind_text(stmt, 2, identity_token, -1, NULL);
    sqlite3_bind_int(stmt, 3, 1);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    db_auth_select(username);
}


int db_auth_select(char *username) {
    if (!db_enabled) { return 0; }
    db_auth_select_none();
    static const char *query =
        "update auth.identity_token set selected = 1 where username = ?;";
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, query, -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, username, -1, NULL);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return sqlite3_changes(db);
}


void db_auth_select_none() {
    if (!db_enabled) { return; }
    sqlite3_exec(db, "update auth.identity_token set selected = 0;",
        NULL, NULL, NULL);
}


int db_auth_get(
    char *username,
    char *identity_token, int identity_token_length)
{
    if (!db_enabled) { return 0; }
    static const char *query =
        "select token from auth.identity_token "
        "where username = ?;";
    int result = 0;
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, query, -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, username, -1, NULL);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *a = (const char *)sqlite3_column_text(stmt, 0);
        strncpy(identity_token, a, identity_token_length - 1);
        identity_token[identity_token_length - 1] = '\0';
        result = 1;
    }
    sqlite3_finalize(stmt);
    return result;
}


int db_auth_get_selected(
    char *username, int username_length,
    char *identity_token, int identity_token_length)
{
    if (!db_enabled) { return 0; }
    static const char *query =
        "select username, token from auth.identity_token "
        "where selected = 1;";
    int result = 0;
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, query, -1, &stmt, NULL);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *a = (const char *)sqlite3_column_text(stmt, 0);
        const char *b = (const char *)sqlite3_column_text(stmt, 1);
        strncpy(username, a, username_length - 1);
        username[username_length - 1] = '\0';
        strncpy(identity_token, b, identity_token_length - 1);
        identity_token[identity_token_length - 1] = '\0';
        result = 1;
    }
    sqlite3_finalize(stmt);
    return result;
}


// Save the player state to the database.
// Arguments:
// - x: x position to save
// - y: y position to save
// - z: z position to save
// - rx: rotation x to save
// - ry: rotation y to save
// - flying: flag for whether the player is flying
void db_save_state(float x, float y, float z, float rx, float ry, int flying) {
    if (!db_enabled) { return; }
    static const char *query =
        "insert into state (x, y, z, rx, ry, flying) values (?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt;
    sqlite3_exec(db, "delete from state;", NULL, NULL, NULL);
    sqlite3_prepare_v2(db, query, -1, &stmt, NULL);
    sqlite3_bind_double(stmt, 1, x);
    sqlite3_bind_double(stmt, 2, y);
    sqlite3_bind_double(stmt, 3, z);
    sqlite3_bind_double(stmt, 4, rx);
    sqlite3_bind_double(stmt, 5, ry);
    sqlite3_bind_int(stmt, 6, flying);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}


// Load the player state from the database.
// Arguments:
// - x: pointer to x position to load value into
// - y: pointer to y position to load value into
// - z: pointer to z position to load value into
// - rx: pointer to rotation x to load value into
// - ry: pointer to rotation y to load value into
// - flying: pointer to flying flag to load value into
// Returns:
// - non-zero if the state entry was successfully found and loaded
int db_load_state(
        float *x, float *y, float *z, float *rx, float *ry, int *flying) 
{
    if (!db_enabled) { return 0; }
    static const char *query =
        "select x, y, z, rx, ry, flying from state;";
    int result = 0;
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, query, -1, &stmt, NULL);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *x = sqlite3_column_double(stmt, 0);
        *y = sqlite3_column_double(stmt, 1);
        *z = sqlite3_column_double(stmt, 2);
        *rx = sqlite3_column_double(stmt, 3);
        *ry = sqlite3_column_double(stmt, 4);
        *flying = sqlite3_column_int(stmt, 5);
        result = 1;
    }
    sqlite3_finalize(stmt);
    return result;
}


// Let one of the workers insert a block into the database.
// Arguments:
// - p, q: chunk x, y position
// - x, y, z: block position
// - w: block id
void db_insert_block(int p, int q, int x, int y, int z, int w) {
    if (!db_enabled) { return; }
    mtx_lock(&mtx);
    ring_put_block(&ring, p, q, x, y, z, w);
    cnd_signal(&cnd);
    mtx_unlock(&mtx);
}


// Actually insert a block into the database.
// Arguments:
// - p: chunk x position
// - q: chunk z position
// - x: block x position
// - y: block y position
// - z: block z position
// - w: block id value
static void _db_insert_block(int p, int q, int x, int y, int z, int w) {
    sqlite3_reset(insert_block_stmt);
    sqlite3_bind_int(insert_block_stmt, 1, p);
    sqlite3_bind_int(insert_block_stmt, 2, q);
    sqlite3_bind_int(insert_block_stmt, 3, x);
    sqlite3_bind_int(insert_block_stmt, 4, y);
    sqlite3_bind_int(insert_block_stmt, 5, z);
    sqlite3_bind_int(insert_block_stmt, 6, w);
    sqlite3_step(insert_block_stmt);
}


void db_insert_block_damage(int p, int q, int x, int y, int z, int damage) {
    if (!db_enabled) { return; }
    mtx_lock(&mtx);
    ring_put_block_damage(&ring, p, q, x, y, z, damage);
    cnd_signal(&cnd);
    mtx_unlock(&mtx);
}


// Actually insert a block damage value into the database
static void _db_insert_block_damage(int p, int q, int x, int y, int z, int w) {
    sqlite3_reset(insert_block_damage_stmt);
    sqlite3_bind_int(insert_block_damage_stmt, 1, p);
    sqlite3_bind_int(insert_block_damage_stmt, 2, q);
    sqlite3_bind_int(insert_block_damage_stmt, 3, x);
    sqlite3_bind_int(insert_block_damage_stmt, 4, y);
    sqlite3_bind_int(insert_block_damage_stmt, 5, z);
    sqlite3_bind_int(insert_block_damage_stmt, 6, w);
    sqlite3_step(insert_block_damage_stmt);
}


void db_trim_block_damage(int p, int q) {
    if (!db_enabled) { return; }
    mtx_lock(&mtx);
    ring_put_block_damage_trim(&ring, p, q);
    cnd_signal(&cnd);
    mtx_unlock(&mtx);
}


// Remove all damage records that just set damage to zero
static void _db_block_damage_trim(int p, int q) {
    sqlite3_reset(trim_block_damage_stmt);
    sqlite3_bind_int(trim_block_damage_stmt, 1, p);
    sqlite3_bind_int(trim_block_damage_stmt, 2, q);
    sqlite3_step(trim_block_damage_stmt);
}


// Let one of the workers insert a light into the database.
// Arguments:
// - p: chunk x position
// - q: chunk z position
// - x: block x position
// - y: block y position
// - z: block z position
// - w: light value
void db_insert_light(int p, int q, int x, int y, int z, int w) {
    if (!db_enabled) { return; }
    mtx_lock(&mtx);
    ring_put_light(&ring, p, q, x, y, z, w);
    cnd_signal(&cnd);
    mtx_unlock(&mtx);
}


// Actually insert a light into the database.
// Arguments:
// - p, q: chunk x, z position
// - x, y, z: light block position
// - w: light value
static void _db_insert_light(int p, int q, int x, int y, int z, int w) {
    sqlite3_reset(insert_light_stmt);
    sqlite3_bind_int(insert_light_stmt, 1, p);
    sqlite3_bind_int(insert_light_stmt, 2, q);
    sqlite3_bind_int(insert_light_stmt, 3, x);
    sqlite3_bind_int(insert_light_stmt, 4, y);
    sqlite3_bind_int(insert_light_stmt, 5, z);
    sqlite3_bind_int(insert_light_stmt, 6, w);
    sqlite3_step(insert_light_stmt);
}


// Insert a sign on the given block and face from the database
// Arguments:
// - p, q: chunk x, z position
// - x, y, z: light block position
// - face: which face of the block the sign is to be on
// - text: the sign's text content
void db_insert_sign(
    int p, int q, int x, int y, int z, int face, const char *text)
{
    if (!db_enabled) { return; }
    sqlite3_reset(insert_sign_stmt);
    sqlite3_bind_int(insert_sign_stmt, 1, p);
    sqlite3_bind_int(insert_sign_stmt, 2, q);
    sqlite3_bind_int(insert_sign_stmt, 3, x);
    sqlite3_bind_int(insert_sign_stmt, 4, y);
    sqlite3_bind_int(insert_sign_stmt, 5, z);
    sqlite3_bind_int(insert_sign_stmt, 6, face);
    sqlite3_bind_text(insert_sign_stmt, 7, text, -1, NULL);
    sqlite3_step(insert_sign_stmt);
}


// Delete a sign on the given block and face from the database
// Arguments:
// - x, y, z: light block position
// - face: which face of the block the sign to delete is on
void db_delete_sign(int x, int y, int z, int face) {
    if (!db_enabled) { return; }
    sqlite3_reset(delete_sign_stmt);
    sqlite3_bind_int(delete_sign_stmt, 1, x);
    sqlite3_bind_int(delete_sign_stmt, 2, y);
    sqlite3_bind_int(delete_sign_stmt, 3, z);
    sqlite3_bind_int(delete_sign_stmt, 4, face);
    sqlite3_step(delete_sign_stmt);
}


// Delete signs on given block from the database
// Arguments:
// - x, y, z: block position
void db_delete_signs(int x, int y, int z) {
    if (!db_enabled) { return; }
    sqlite3_reset(delete_signs_stmt);
    sqlite3_bind_int(delete_signs_stmt, 1, x);
    sqlite3_bind_int(delete_signs_stmt, 2, y);
    sqlite3_bind_int(delete_signs_stmt, 3, z);
    sqlite3_step(delete_signs_stmt);
}


// Delete all signs from the database
void db_delete_all_signs() {
    if (!db_enabled) { return; }
    sqlite3_exec(db, "delete from sign;", NULL, NULL, NULL);
}


// Load all of the blocks from database in chunk
// Arguments:
// - map: block map destination to load block values into
// - p, q: chunk x, z position
// Returns: none
void db_load_blocks(Map *map, int p, int q) {
    if (!db_enabled) { return; }
    mtx_lock(&load_mtx);
    sqlite3_reset(load_blocks_stmt);
    sqlite3_bind_int(load_blocks_stmt, 1, p);
    sqlite3_bind_int(load_blocks_stmt, 2, q);
    while (sqlite3_step(load_blocks_stmt) == SQLITE_ROW) {
        int x = sqlite3_column_int(load_blocks_stmt, 0);
        int y = sqlite3_column_int(load_blocks_stmt, 1);
        int z = sqlite3_column_int(load_blocks_stmt, 2);
        int w = sqlite3_column_int(load_blocks_stmt, 3);
        map_set(map, x, y, z, w);
    }
    mtx_unlock(&load_mtx);
}


// Load all of the block damage from database in chunk
// Arguments:
// - map: block damage map destination to load block values into
// - p: chunk x position
// - q: chunk z position
// Returns: none
void db_load_damage(Map *map, int p, int q) {
    if (!db_enabled) { return; }
    mtx_lock(&load_mtx);
    sqlite3_reset(load_block_damage_stmt);
    sqlite3_bind_int(load_block_damage_stmt, 1, p);
    sqlite3_bind_int(load_block_damage_stmt, 2, q);
    while (sqlite3_step(load_block_damage_stmt) == SQLITE_ROW) {
        int x = sqlite3_column_int(load_block_damage_stmt, 0);
        int y = sqlite3_column_int(load_block_damage_stmt, 1);
        int z = sqlite3_column_int(load_block_damage_stmt, 2);
        int d = sqlite3_column_int(load_block_damage_stmt, 3);
        if (!d) { continue; }
        map_set(map, x, y, z, d);
    }
    mtx_unlock(&load_mtx);
}


// Load all of the lights from database in chunk
// Arguments:
// - map: light map pointer to output light values into
// - p: chunk x position
// - q: chunk z position
// Returns:
// - modifies lights in given map pointer
void db_load_lights(Map *map, int p, int q) {
    if (!db_enabled) { return; }
    mtx_lock(&load_mtx);
    sqlite3_reset(load_lights_stmt);
    sqlite3_bind_int(load_lights_stmt, 1, p);
    sqlite3_bind_int(load_lights_stmt, 2, q);
    while (sqlite3_step(load_lights_stmt) == SQLITE_ROW) {
        int x = sqlite3_column_int(load_lights_stmt, 0);
        int y = sqlite3_column_int(load_lights_stmt, 1);
        int z = sqlite3_column_int(load_lights_stmt, 2);
        int w = sqlite3_column_int(load_lights_stmt, 3);
        map_set(map, x, y, z, w);
    }
    mtx_unlock(&load_mtx);
}


// Load all of the signs from database in chunk
// Arguments:
// - list: pointer to output list of signs into
// - p: chunk x position
// - q: chunk z position
// Returns:
// - adds sign entries to list
void db_load_signs(SignList *list, int p, int q) {
    if (!db_enabled) { return; } sqlite3_reset(load_signs_stmt);
    sqlite3_bind_int(load_signs_stmt, 1, p);
    sqlite3_bind_int(load_signs_stmt, 2, q);
    while (sqlite3_step(load_signs_stmt) == SQLITE_ROW) {
        int x = sqlite3_column_int(load_signs_stmt, 0);
        int y = sqlite3_column_int(load_signs_stmt, 1);
        int z = sqlite3_column_int(load_signs_stmt, 2);
        int face = sqlite3_column_int(load_signs_stmt, 3);
        const char *text = (const char *)sqlite3_column_text(
            load_signs_stmt, 4);
        sign_list_add(list, x, y, z, face, text);
    }
}


// Get the key value for the chunk at the given position
// Arguments:
// - p: chunk x position
// - q: chunk z position
// Returns:
// - key value
int db_get_key(int p, int q) {
    if (!db_enabled) { return 0; }
    sqlite3_reset(get_key_stmt);
    sqlite3_bind_int(get_key_stmt, 1, p);
    sqlite3_bind_int(get_key_stmt, 2, q);
    if (sqlite3_step(get_key_stmt) == SQLITE_ROW) {
        return sqlite3_column_int(get_key_stmt, 0);
    }
    return 0;
}


// Let one of the workers set a key for a chunk
// Arguments:
// - p: chunk x position
// - q: chunk z position
// - key: ket to be set for the chunk
// Returns: none
void db_set_key(int p, int q, int key) {
    if (!db_enabled) { return; }
    mtx_lock(&mtx);
    ring_put_key(&ring, p, q, key);
    cnd_signal(&cnd);
    mtx_unlock(&mtx);
}


// Actually set a key for a chunk
// Arguments:
// - p: chunk x position
// - q: chunk z position
// - key: ket to be set for the chunk
// Returns: none
static void _db_set_key(int p, int q, int key) {
    sqlite3_reset(set_key_stmt);
    sqlite3_bind_int(set_key_stmt, 1, p);
    sqlite3_bind_int(set_key_stmt, 2, q);
    sqlite3_bind_int(set_key_stmt, 3, key);
    sqlite3_step(set_key_stmt);
}


// Start a worker with the database
// Arguments:
// - path: argument to pass to the worker
// Returns: none
void db_worker_start(char *path) {
    if (!db_enabled) { return; }
    ring_alloc(&ring, 1024);
    mtx_init(&mtx, mtx_plain);
    mtx_init(&load_mtx, mtx_plain);
    cnd_init(&cnd);
    thrd_create(&thrd, db_worker_run, path);
}


// Stop workers from using the database.
// Arguments: none
// Returns: none
void db_worker_stop() {
    if (!db_enabled) { return; }
    mtx_lock(&mtx);
    ring_put_exit(&ring);
    cnd_signal(&cnd);
    mtx_unlock(&mtx);
    thrd_join(thrd, NULL);
    cnd_destroy(&cnd);
    mtx_destroy(&load_mtx);
    mtx_destroy(&mtx);
    ring_free(&ring);
}


// This is where a worker will fetch and perform database operations.
// Arguments:
// - arg: unused in this function
// Returns:
// - 0
int db_worker_run(void * /*arg*/) {
    int running = 1;
    while (running) {
        RingEntry e;
        mtx_lock(&mtx);
        while (!ring_get(&ring, &e)) {
            cnd_wait(&cnd, &mtx);
        }
        mtx_unlock(&mtx);
        switch (e.type) {
            case BLOCK:
                _db_insert_block(e.p, e.q, e.x, e.y, e.z, e.w);
                _db_insert_block_damage(e.p, e.q, e.x, e.y, e.z, 0);
                break;
            case LIGHT:
                _db_insert_light(e.p, e.q, e.x, e.y, e.z, e.w);
                break;
            case KEY:
                _db_set_key(e.p, e.q, e.key);
                break;
            case COMMIT:
                _db_commit();
                break;
            case EXIT:
                running = 0;
                break;
            case BLOCK_DAMAGE:
                _db_insert_block_damage(e.p, e.q, e.x, e.y, e.z, e.w);
                break;
            case BLOCK_DAMAGE_TRIM:
                _db_block_damage_trim(e.p, e.q);
                break;
        }
    }
    return 0;
}

