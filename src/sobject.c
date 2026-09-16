// James Ezra Seitenschlag
// 11.09.2026
//
// nadir runtime thingy

#include "sobject.h"
#include "metadata.h"
#include "nadir_hash.h"
#include <time.h>

// -----------------------------------------------------------------------------
// Bounded ASCII helpers
//
// Field lookup runs once per field per row, so strcasecmp's locale handling is
// pure overhead here. These fold ASCII only, which is all Apex metadata and
// API names ever contain.
// -----------------------------------------------------------------------------

static inline unsigned char nadr_fold(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c;
}

static inline bool field_name_eq(const char* a, const char* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    // Cheap rejection before walking the string
    if (a[0] != b[0] && nadr_fold((unsigned char)a[0]) != nadr_fold((unsigned char)b[0])) return false;
    while (*a && *b) {
        if (nadr_fold((unsigned char)*a) != nadr_fold((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static inline bool field_name_eq_n(const char* a, const char* b, size_t b_len) {
    if (!a || !b) return false;
    for (size_t i = 0; i < b_len; i++) {
        if (a[i] == '\0') return false;
        if (nadr_fold((unsigned char)a[i]) != nadr_fold((unsigned char)b[i])) return false;
    }
    return a[b_len] == '\0';
}

// -----------------------------------------------------------------------------
// Field name interning
//
// A bulk SOQL projection builds `SELECT Id, Name, BillingCity ...` for every
// matched row. Without interning that is one malloc+copy per field per row;
// with it, the first row pays for the name and every later row shares the
// pointer. The table is open-addressed and never shrinks: metadata field names
// are a small, bounded vocabulary.
// -----------------------------------------------------------------------------

typedef struct InternEntry {
    char* name;
    uint32_t hash;
    bool occupied;
} InternEntry;

#define INTERN_MIN_CAPACITY 256

static InternEntry* g_intern = NULL;
static int g_intern_capacity = 0;
static int g_intern_count = 0;

static uint32_t intern_hash(const char* s, size_t len) {
    return nadr_hash_fold(s, len);
}

static void intern_grow(void) {
    int new_cap = g_intern_capacity ? g_intern_capacity * 2 : INTERN_MIN_CAPACITY;
    InternEntry* old = g_intern;
    int old_cap = g_intern_capacity;

    g_intern = (InternEntry*)calloc((size_t)new_cap, sizeof(InternEntry));
    if (!g_intern) abort();
    g_intern_capacity = new_cap;
    g_intern_count = 0;

    // Only the bucket array is reallocated here. The name strings themselves
    // are permanent allocations, because records already handed out to Apex
    // hold pointers to them; freeing the old bucket array must not invalidate
    // those names.
    for (int i = 0; i < old_cap; i++) {
        if (!old[i].occupied) continue;
        uint32_t mask = (uint32_t)(new_cap - 1);
        uint32_t idx = old[i].hash & mask;
        while (g_intern[idx].occupied) idx = (idx + 1) & mask;
        g_intern[idx] = old[i];
        g_intern_count++;
    }
    if (old) free(old);
}

const char* sobject_intern_field_slice(const char* name, size_t len) {
    if (!name) return NULL;
    if (g_intern_capacity == 0 || (g_intern_count + 1) * 4 >= g_intern_capacity * 3) {
        intern_grow();
    }
    uint32_t h = intern_hash(name, len);
    uint32_t mask = (uint32_t)(g_intern_capacity - 1);
    uint32_t idx = h & mask;
    while (g_intern[idx].occupied) {
        if (g_intern[idx].hash == h && field_name_eq_n(g_intern[idx].name, name, len)) {
            return g_intern[idx].name;
        }
        idx = (idx + 1) & mask;
    }
    // Permanent copy: never freed for the lifetime of the process.
    char* copy = nadr_strndup(name, (int)len);
    g_intern[idx].name = copy;
    g_intern[idx].hash = h;
    g_intern[idx].occupied = true;
    g_intern_count++;
    return copy;
}

// Frees the bucket array only. The interned names are intentionally leaked:
// they are stable-address, process-lifetime string constants in practice.
static void intern_table_release(void) {
    if (g_intern) free(g_intern);
    g_intern = NULL;
    g_intern_capacity = 0;
    g_intern_count = 0;
}

const char* sobject_intern_field(const char* name) {
    if (!name) return NULL;
    return sobject_intern_field_slice(name, strlen(name));
}

// -----------------------------------------------------------------------------
// Field index
//
// Records with few fields are faster to scan linearly (the probe usually hits
// the first or second entry) so the index is only materialised once a record
// grows past SOBJECT_INDEX_THRESHOLD. Past that point every sobject_get on the
// record costs one hash instead of up to N case-insensitive compares.
// -----------------------------------------------------------------------------

#define SOBJECT_INDEX_THRESHOLD 8
#define SOBJECT_INDEX_MIN_CAPACITY 16

static void sobject_index_rebuild(SObject* obj, int capacity) {
    if (capacity < SOBJECT_INDEX_MIN_CAPACITY) capacity = SOBJECT_INDEX_MIN_CAPACITY;
    int32_t* idx = (int32_t*)calloc((size_t)capacity, sizeof(int32_t));
    if (!idx) abort();
    if (obj->field_index) free(obj->field_index);
    obj->field_index = idx;
    obj->field_index_capacity = capacity;
    uint32_t mask = (uint32_t)(capacity - 1);
    for (int i = 0; i < obj->field_count; i++) {
        uint32_t pos = obj->fields[i].hash & mask;
        while (idx[pos] != 0) pos = (pos + 1) & mask;
        idx[pos] = i + 1;
        obj->fields[i].slot = (int32_t)pos;
    }
}

static void sobject_index_maybe_init(SObject* obj) {
    if (obj->field_index || obj->field_count < SOBJECT_INDEX_THRESHOLD) return;
    int cap = SOBJECT_INDEX_MIN_CAPACITY;
    while (cap < obj->field_count * 2) cap *= 2;
    sobject_index_rebuild(obj, cap);
}

// Append a field known to be absent. Rebuilds the index when load crosses 3/4.
static void sobject_append_field(SObject* obj, char* name, uint32_t hash, Value val) {
    if (obj->field_count + 1 > obj->field_capacity) {
        obj->field_capacity = obj->field_capacity < 8 ? 8 : obj->field_capacity * 2;
        obj->fields = (SObjectField*)realloc(obj->fields, sizeof(SObjectField) * (size_t)obj->field_capacity);
        if (!obj->fields) abort();
    }
    int at = obj->field_count;
    obj->fields[at].name = name;
    obj->fields[at].hash = hash;
    obj->fields[at].slot = -1;
    obj->fields[at].value = val;
    obj->field_count++;

    if (obj->field_index) {
        if ((obj->field_count + 1) * 4 >= obj->field_index_capacity * 3) {
            sobject_index_rebuild(obj, obj->field_index_capacity * 2);
        } else {
            uint32_t mask = (uint32_t)(obj->field_index_capacity - 1);
            uint32_t pos = hash & mask;
            while (obj->field_index[pos] != 0) pos = (pos + 1) & mask;
            obj->field_index[pos] = at + 1;
            obj->fields[at].slot = (int32_t)pos;
        }
    } else {
        sobject_index_maybe_init(obj);
    }
}

// Find the storage position of a folded-hash-matched field, or -1.
static inline int sobject_locate(SObject* obj, const char* name, uint32_t hash, size_t len) {
    if (obj->field_index) {
        uint32_t mask = (uint32_t)(obj->field_index_capacity - 1);
        uint32_t pos = hash & mask;
        while (obj->field_index[pos] != 0) {
            int at = obj->field_index[pos] - 1;
            if (obj->fields[at].hash == hash) {
                // hash equal is not proof equal, confirm without a strlen walk
                if (field_name_eq_n(obj->fields[at].name, name, len)) return at;
            }
            pos = (pos + 1) & mask;
        }
        return -1;
    }
    for (int i = 0; i < obj->field_count; i++) {
        if (obj->fields[i].hash == hash && field_name_eq(obj->fields[i].name, name)) return i;
    }
    return -1;
}

uint32_t sobject_field_hash(const char* name) {
    if (!name) return 0;
    return nadr_hash_fold(name, strlen(name));
}

SObject* sobject_new(const char* type_name) {
    SObject* o = (SObject*)malloc(sizeof(SObject));
    if (!o) {
        fprintf(stderr, "FATAL: alloc sobject failed for '%s'\n", type_name ? type_name : "null");
        abort();
    }
    o->type_name = nadr_strdup(type_name);
    o->fields = NULL;
    o->field_count = 0;
    o->field_capacity = 0;
    o->is_deleted = false;
    o->field_index = NULL;
    o->field_index_capacity = 0;
    o->names_interned = false;
    return o;
}

void sobject_reserve_fields(SObject* obj, int capacity) {
    if (!obj || capacity <= obj->field_capacity) return;
    obj->fields = (SObjectField*)realloc(obj->fields, sizeof(SObjectField) * (size_t)capacity);
    if (!obj->fields) abort();
    obj->field_capacity = capacity;
}

void sobject_put_prehashed(SObject* obj, const char* name, uint32_t hash, size_t len, Value val) {
    if (!obj || !name) return;
    int at = sobject_locate(obj, name, hash, len);
    if (at >= 0) {
        obj->fields[at].value = val;
        return;
    }
    sobject_append_field(obj, nadr_strdup(name), hash, val);
}

void sobject_put(SObject* obj, const char* field_name, Value val) {
    if (!obj || !field_name) return;
    sobject_put_prehashed(obj, field_name, nadr_hash_fold(field_name, strlen(field_name)),
                          strlen(field_name), val);
}

// Build a record with a known field count; slots are filled positionally by
// sobject_set_field_at, so no index is maintained while projecting.
SObject* sobject_new_projected(const char* type_name, int count) {
    SObject* o = sobject_new(type_name);
    if (count > 0) {
        o->fields = (SObjectField*)calloc((size_t)count, sizeof(SObjectField));
        if (!o->fields) abort();
        o->field_capacity = count;
    }
    o->names_interned = true;
    return o;
}

void sobject_set_field_at(SObject* obj, int at, char* name, uint32_t hash, Value val) {
    if (!obj || at < 0 || at >= obj->field_capacity) return;
    obj->fields[at].name = name;
    obj->fields[at].hash = hash;
    obj->fields[at].slot = -1;
    obj->fields[at].value = val;
    if (at >= obj->field_count) obj->field_count = at + 1;
}

int sobject_field_slot(SObject* obj, const char* name) {
    if (!obj || !name) return -1;
    size_t len = strlen(name);
    return sobject_locate(obj, name, nadr_hash_fold(name, len), len);
}

Value sobject_field_at(SObject* obj, int slot) {
    if (!obj || slot < 0 || slot >= obj->field_count) return val_null();
    return obj->fields[slot].value;
}

Value sobject_get(SObject* obj, const char* field_name) {
    if (!obj || !field_name) return val_null();
    size_t len = strlen(field_name);
    int at = sobject_locate(obj, field_name, nadr_hash_fold(field_name, len), len);
    return at >= 0 ? obj->fields[at].value : val_null();
}

SObject* sobject_clone(SObject* src) {
    if (!src) return NULL;
    SObject* c = sobject_new(src->type_name);
    sobject_reserve_fields(c, src->field_count);
    // Manual copy: the field table is already de-duplicated and any index is
    // rebuilt lazily, so cloning does not need repeated name hashing.
    for (int i = 0; i < src->field_count; i++) {
        c->fields[i].name = nadr_strdup(src->fields[i].name);
        c->fields[i].hash = src->fields[i].hash;
        c->fields[i].slot = -1;
        c->fields[i].value = src->fields[i].value;
    }
    c->field_count = src->field_count;
    c->is_deleted = src->is_deleted;
    return c;
}

void sobject_free(SObject* obj) {
    if (!obj) return;
    if (obj->type_name) free(obj->type_name);
    if (obj->fields) {
        // Interned names are owned by the intern table, not by the record.
        if (!obj->names_interned) {
            for (int f = 0; f < obj->field_count; f++) {
                if (obj->fields[f].name) free(obj->fields[f].name);
            }
        }
        free(obj->fields);
    }
    if (obj->field_index) free(obj->field_index);
    free(obj);
}

// mock in-memory db table storage
static MockDB g_mock_db = {NULL, 0, 0, NULL, 0, 0, 1001};

MockDB* mock_db_get_instance(void) {
    return &g_mock_db;
}

static DBTable* get_or_create_table(MockDB* db, const char* object_name) {
    for (int i = 0; i < db->table_count; i++) {
        if (nadr_str_eq(db->tables[i].object_name, object_name)) {
            return &db->tables[i];
        }
    }
    if (db->table_count + 1 > db->table_capacity) {
        db->table_capacity = db->table_capacity < 8 ? 8 : db->table_capacity * 2;
        db->tables = (DBTable*)realloc(db->tables, sizeof(DBTable) * (size_t)db->table_capacity);
        if (!db->tables) abort();
    }
    DBTable* table = &db->tables[db->table_count++];
    table->object_name = nadr_strdup(object_name);
    table->records = NULL;
    table->record_count = 0;
    table->record_capacity = 0;
    table->auto_id_seq = 0;
    return table;
}

// 3-char standard sfdc prefixes
const char* get_sfdc_prefix(const char* obj_name) {
    if (nadr_str_eq(obj_name, "Account"))      return "001";
    if (nadr_str_eq(obj_name, "Contact"))      return "003";
    if (nadr_str_eq(obj_name, "Opportunity"))  return "006";
    if (nadr_str_eq(obj_name, "Lead"))         return "00Q";
    if (nadr_str_eq(obj_name, "Task"))         return "00T";
    if (nadr_str_eq(obj_name, "Event"))        return "00U";
    if (nadr_str_eq(obj_name, "Case"))         return "500";
    if (nadr_str_eq(obj_name, "User"))         return "005";
    if (nadr_str_eq(obj_name, "Order"))        return "801";
    if (nadr_str_eq(obj_name, "OrderItem"))    return "802";
    if (nadr_str_eq(obj_name, "Product2"))     return "01t";
    if (nadr_str_eq(obj_name, "Pricebook2"))   return "01s";
    if (nadr_str_eq(obj_name, "Campaign"))     return "701";
    if (nadr_str_eq(obj_name, "Document"))     return "015";
    if (nadr_str_eq(obj_name, "Attachment"))   return "00P";
    return "a00"; // custom objects default
}

// 15 to 18 char id checksum
static void compute_sfdc_checksum(const char* id15, char* out_suffix) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";
    for (int chunk = 0; chunk < 3; chunk++) {
        int mask = 0;
        for (int i = 0; i < 5; i++) {
            char c = id15[chunk * 5 + i];
            if (c >= 'A' && c <= 'Z') {
                mask |= (1 << i);
            }
        }
        out_suffix[chunk] = tbl[mask];
    }
    out_suffix[3] = '\0';
}

// required fields check
//
// Walks the *record's* field table once and cross-checks it against the
// required names, rather than issue one sobject_get per required field (which
// re-scanned the record every time). Called once per row, not per field.
static bool validate_sfdc_schema_rules(SObject* obj, char* err_buf, size_t err_sz) {
    if (!obj) return false;

    static const char* const account_required[] = {"Name"};
    static const char* const contact_required[] = {"LastName"};
    const char* const* required = NULL;
    int required_count = 0;

    if (nadr_str_eq(obj->type_name, "Account")) {
        required = account_required;
        required_count = 1;
    } else if (nadr_str_eq(obj->type_name, "Contact")) {
        required = contact_required;
        required_count = 1;
    }

    for (int r = 0; r < required_count; r++) {
        int at = sobject_field_slot(obj, required[r]);
        bool missing = true;
        if (at >= 0) {
            Value v = obj->fields[at].value;
            if (v.type == VAL_STRING) missing = (v.as.string_val == NULL || v.as.string_val[0] == '\0');
            else missing = (v.type == VAL_NULL);
        }
        if (missing) {
            snprintf(err_buf, err_sz, "DmlException: Required fields are missing: [%s]", required[r]);
            return false;
        }
    }

    MetaObject* meta = project_schema_get_object(obj->type_name);
    if (meta) {
        for (int f = 0; f < meta->field_count; f++) {
            if (!meta->fields[f].required) continue;
            int at = sobject_field_slot(obj, meta->fields[f].full_name);
            bool missing = true;
            if (at >= 0) {
                Value fv = obj->fields[at].value;
                if (fv.type == VAL_STRING) missing = (fv.as.string_val == NULL || fv.as.string_val[0] == '\0');
                else missing = (fv.type == VAL_NULL);
            }
            if (missing) {
                snprintf(err_buf, err_sz, "DmlException: Required fields are missing: [%s]", meta->fields[f].full_name);
                return false;
            }
        }
    }
    return true;
}

// -----------------------------------------------------------------------------
// Transaction clock
//
// One DML statement is one transaction in Apex, so every row it writes shares
// a single CreatedDate/LastModifiedDate. The previous code called time() and
// localtime() per row; localtime() in particular is a locking, timezone-aware
// call and completely dominated the per-row insert cost.
// -----------------------------------------------------------------------------

static time_t g_last_time_sec = 0;
static char g_cached_date_buf[64] = {0};

void format_timestamp(time_t now, char* out, size_t out_sz) {
    if (now == g_last_time_sec && g_cached_date_buf[0] != '\0') {
        strncpy(out, g_cached_date_buf, out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }
    struct tm tm_info;
#if defined(_WIN32)
    localtime_s(&tm_info, &now);
#else
    localtime_r(&now, &tm_info);
#endif
    strftime(g_cached_date_buf, sizeof(g_cached_date_buf), "%Y-%m-%d %H:%M:%S", &tm_info);
    g_last_time_sec = now;
    strncpy(out, g_cached_date_buf, out_sz - 1);
    out[out_sz - 1] = '\0';
}

static const char* g_intern_id = "Id";
static const char* g_intern_created = "CreatedDate";
static const char* g_intern_modified = "LastModifiedDate";
static const char* g_intern_deleted = "IsDeleted";
static uint32_t g_hash_id = 0;
static uint32_t g_hash_created = 0;
static uint32_t g_hash_modified = 0;
static uint32_t g_hash_deleted = 0;

static void ensure_system_field_hashes(void) {
    if (g_hash_id == 0) {
        g_hash_id = nadr_hash_fold(g_intern_id, 2);
        g_hash_created = nadr_hash_fold(g_intern_created, 11);
        g_hash_modified = nadr_hash_fold(g_intern_modified, 16);
        g_hash_deleted = nadr_hash_fold(g_intern_deleted, 9);
    }
}

static Value mock_db_insert_with_time(SObject* obj, DBTable* table, const char* date_buf, int64_t next_seq) {
    char id15[16];
    char id_full[32];
    const char* pfx = get_sfdc_prefix(obj->type_name);

    snprintf(id15, sizeof(id15), "%s%012lld", pfx, (long long)next_seq);

    char sfx[4];
    compute_sfdc_checksum(id15, sfx);
    snprintf(id_full, sizeof(id_full), "%s%s", id15, sfx);

    ensure_system_field_hashes();
    sobject_put_prehashed(obj, g_intern_id, g_hash_id, 2, val_string(id_full));
    sobject_put_prehashed(obj, g_intern_created, g_hash_created, 11, val_string(date_buf));
    sobject_put_prehashed(obj, g_intern_modified, g_hash_modified, 16, val_string(date_buf));
    sobject_put_prehashed(obj, g_intern_deleted, g_hash_deleted, 9, val_bool(false));

    if (table->record_count + 1 > table->record_capacity) {
        table->record_capacity = table->record_capacity < 8 ? 8 : table->record_capacity * 2;
        table->records = (SObject**)realloc(table->records, sizeof(SObject*) * (size_t)table->record_capacity);
        if (!table->records) abort();
    }
    table->records[table->record_count++] = obj;
    return val_sobject(obj);
}

static int64_t g_global_record_id = 0;

Value mock_db_insert(SObject* obj) {
    if (!obj) return val_null();

    char err[256];
    if (!validate_sfdc_schema_rules(obj, err, sizeof(err))) {
        fprintf(stderr, "%s\n", err);
        return val_null();
    }

    MockDB* db = mock_db_get_instance();
    DBTable* table = get_or_create_table(db, obj->type_name);

    char date_buf[64];
    format_timestamp(time(NULL), date_buf, sizeof(date_buf));

    table->auto_id_seq = (int)(++g_global_record_id);
    return mock_db_insert_with_time(obj, table, date_buf, g_global_record_id);
}

void mock_db_insert_bulk(SObject** records, int count) {
    if (!records || count <= 0) return;

    // Resolve the table once per statement; every row targets the same object.
    MockDB* db = mock_db_get_instance();
    DBTable* table = get_or_create_table(db, records[0]->type_name);

    // Reserve capacity up front so the row loop never reallocs.
    if (table->record_count + count > table->record_capacity) {
        int cap = table->record_capacity < 8 ? 8 : table->record_capacity;
        while (cap < table->record_count + count) cap *= 2;
        table->records = (SObject**)realloc(table->records, sizeof(SObject*) * (size_t)cap);
        if (!table->records) abort();
        table->record_capacity = cap;
    }

    char date_buf[64];
    format_timestamp(time(NULL), date_buf, sizeof(date_buf));

    char err[256];
    for (int i = 0; i < count; i++) {
        SObject* obj = records[i];
        if (!obj) continue;
        if (obj->is_deleted) continue;
        if (!validate_sfdc_schema_rules(obj, err, sizeof(err))) {
            fprintf(stderr, "%s\n", err);
            continue;
        }
        g_global_record_id++;
        mock_db_insert_with_time(obj, table, date_buf, g_global_record_id);
    }
    table->auto_id_seq = (int)g_global_record_id;
}

// Locate a record by id. Ids have a fixed 18 char shape, so compare on the
// first byte before the full string.
static int table_find_by_id(DBTable* table, const char* id) {
    if (!id) return -1;
    char first = id[0];
    for (int i = 0; i < table->record_count; i++) {
        SObject* rec = table->records[i];
        if (rec->is_deleted) continue;
        int at = sobject_field_slot(rec, "Id");
        if (at < 0) continue;
        Value v = rec->fields[at].value;
        if (v.type != VAL_STRING || !v.as.string_val) continue;
        if (v.as.string_val[0] != first) continue;
        if (nadr_str_eq(v.as.string_val, id)) return i;
    }
    return -1;
}

Value mock_db_update(SObject* obj) {
    if (!obj) return val_null();
    Value id_val = sobject_get(obj, "Id");
    if (id_val.type != VAL_STRING) {
        fprintf(stderr, "DmlException: Cannot update record without valid 18-char Id\n");
        return val_null();
    }
    MockDB* db = mock_db_get_instance();
    DBTable* table = get_or_create_table(db, obj->type_name);

    int at = table_find_by_id(table, id_val.as.string_val);
    if (at >= 0) {
        table->records[at] = obj;
        return val_sobject(obj);
    }
    fprintf(stderr, "DmlException: Record with Id '%s' not found in database\n", id_val.as.string_val);
    return val_null();
}

void mock_db_update_bulk(SObject** records, int count) {
    if (!records || count <= 0) return;
    MockDB* db = mock_db_get_instance();
    DBTable* table = get_or_create_table(db, records[0]->type_name);
    for (int i = 0; i < count; i++) {
        SObject* obj = records[i];
        if (!obj || obj->is_deleted) continue;
        Value id_val = sobject_get(obj, "Id");
        if (id_val.type != VAL_STRING) {
            fprintf(stderr, "DmlException: Cannot update record without valid 18-char Id\n");
            continue;
        }
        int at = table_find_by_id(table, id_val.as.string_val);
        if (at >= 0) {
            table->records[at] = obj;
        } else {
            fprintf(stderr, "DmlException: Record with Id '%s' not found in database\n", id_val.as.string_val);
        }
    }
}

// Physical remove via swap-with-last. The previous implementation shifted the
// whole tail on every delete, which turned a bulk delete of N rows into O(N^2)
// pointer moves. Record order is not part of the observable Apex contract.
static void table_remove_at(DBTable* table, int at) {
    SObject* victim = table->records[at];
    if (victim) victim->is_deleted = true;
    int last = table->record_count - 1;
    if (at != last) {
        table->records[at] = table->records[last];
    }
    table->record_count = last;
}

Value mock_db_delete(SObject* obj) {
    if (!obj) return val_null();
    Value id_val = sobject_get(obj, "Id");
    if (id_val.type != VAL_STRING) return val_null();

    MockDB* db = mock_db_get_instance();
    DBTable* table = get_or_create_table(db, obj->type_name);

    int at = table_find_by_id(table, id_val.as.string_val);
    if (at >= 0) {
        table_remove_at(table, at);
        return val_sobject(obj);
    }
    return val_null();
}

void mock_db_delete_bulk(SObject** records, int count) {
    if (!records || count <= 0) return;
    MockDB* db = mock_db_get_instance();
    DBTable* table = get_or_create_table(db, records[0]->type_name);
    for (int i = 0; i < count; i++) {
        SObject* obj = records[i];
        if (!obj || obj->is_deleted) continue;
        Value id_val = sobject_get(obj, "Id");
        if (id_val.type != VAL_STRING) continue;
        int at = table_find_by_id(table, id_val.as.string_val);
        if (at >= 0) table_remove_at(table, at);
    }
}

void mock_db_dml_bulk(const char* operation, SObject** records, int count) {
    if (!records || count <= 0 || !operation) return;
    char op0 = operation[0];
    if (op0 == 'i' || op0 == 'I' || op0 == 'u' || op0 == 'U') {
        // insert / upsert / update all share the leading letter pair test below
        if ((op0 == 'i' || op0 == 'I') && (operation[1] == 'n' || operation[1] == 'N')) {
            mock_db_insert_bulk(records, count);
            return;
        }
        if (op0 == 'u' || op0 == 'U') {
            if (operation[1] == 'p' || operation[1] == 'P') {
                mock_db_insert_bulk(records, count); // upsert inserts in this mock
                return;
            }
            if (operation[1] == 'n' || operation[1] == 'N') {
                mock_db_update_bulk(records, count);
                return;
            }
        }
    }
    if ((op0 == 'd' || op0 == 'D') && (operation[1] == 'e' || operation[1] == 'E')) {
        mock_db_delete_bulk(records, count);
        return;
    }
    if ((op0 == 'm' || op0 == 'M') && (operation[1] == 'e' || operation[1] == 'E')) {
        return; // merge is a no-op in this mock
    }
}

// -----------------------------------------------------------------------------
// SOQL
// -----------------------------------------------------------------------------

// Comparison kinds, resolved once from the operator string.
enum {
    QOP_NONE = 0,
    QOP_EQ,
    QOP_NE,
    QOP_GT,
    QOP_LT,
    QOP_GE,
    QOP_LE
};

static int resolve_where_op(const char* op) {
    if (!op || !op[0]) return QOP_NONE;
    if (op[0] == '=' ) return QOP_EQ;
    if (op[0] == '!' ) return QOP_NE;
    if (op[0] == '<' ) return op[1] == '>' ? QOP_NE : (op[1] == '=' ? QOP_LE : QOP_LT);
    if (op[0] == '>' ) return op[1] == '=' ? QOP_GE : QOP_GT;
    if ((op[0] == 'l' || op[0] == 'L')) return QOP_EQ; // LIKE falls back to equality
    return QOP_NONE;
}

SObjectQueryPlan* sobject_query_plan_build(DBTable* table, const char** fields, int field_count,
                                           const char* where_field, const char* where_op) {
    size_t sz = sizeof(SObjectQueryPlan) + sizeof(SObjectFieldSlot) * (size_t)(field_count > 0 ? field_count - 1 : 0);
    SObjectQueryPlan* plan = (SObjectQueryPlan*)calloc(1, sz);
    if (!plan) abort();

    plan->field_count = field_count;
    plan->where_slot = -1;
    plan->where_kind = resolve_where_op(where_op);

    // Resolve against a live representative record when the table is populated.
    // Shapes are uniform per table, so one probe answers the whole query; an
    // empty table simply leaves every slot unresolved and the row loop falls
    // back to name lookup (there are no rows to iterate anyway).
    SObject* sample = (table && table->record_count > 0) ? table->records[0] : NULL;

    for (int i = 0; i < field_count; i++) {
        const char* n = fields[i];
        plan->fields[i].name = (char*)n;
        plan->fields[i].hash = sobject_field_hash(n);
        plan->fields[i].slot = sample ? sobject_locate(sample, n, plan->fields[i].hash, strlen(n)) : -1;
    }

    if (where_field && plan->where_kind != QOP_NONE) {
        plan->where_slot = sample ? sobject_locate(sample, where_field, sobject_field_hash(where_field),
                                                   strlen(where_field)) : -1;
    }
    return plan;
}

void sobject_query_plan_free(SObjectQueryPlan* plan) {
    if (plan) free(plan);
}

// Row predicate, hoisted out of the scan loop.
static inline bool query_row_matches(SObject* src, SObjectQueryPlan* plan, Value where_val) {
    if (plan->where_kind == QOP_NONE || plan->where_slot < 0) return true;
    Value actual = sobject_field_at(src, plan->where_slot);
    switch (plan->where_kind) {
        case QOP_EQ: return val_equals(actual, where_val);
        case QOP_NE: return !val_equals(actual, where_val);
        case QOP_GT:
        case QOP_LT:
        case QOP_GE:
        case QOP_LE: {
            bool ai = (actual.type == VAL_INT);
            bool ad = (actual.type == VAL_DOUBLE);
            bool wi = (where_val.type == VAL_INT);
            bool wd = (where_val.type == VAL_DOUBLE);
            if ((ai || ad) && (wi || wd)) {
                double da = ad ? actual.as.double_val : (double)actual.as.int_val;
                double db = wd ? where_val.as.double_val : (double)where_val.as.int_val;
                switch (plan->where_kind) {
                    case QOP_GT: return da > db;
                    case QOP_LT: return da < db;
                    case QOP_GE: return da >= db;
                    default:     return da <= db;
                }
            }
            if (actual.type == VAL_STRING && where_val.type == VAL_STRING) {
                int c = strcasecmp(actual.as.string_val, where_val.as.string_val);
                switch (plan->where_kind) {
                    case QOP_GT: return c > 0;
                    case QOP_LT: return c < 0;
                    case QOP_GE: return c >= 0;
                    default:     return c <= 0;
                }
            }
            return false;
        }
        default: return true;
    }
}

Value mock_db_query(const char* from_obj, const char** fields, int field_count,
                    const char* where_field, const char* where_op, Value where_val, int limit) {
    MockDB* db = mock_db_get_instance();
    DBTable* table = get_or_create_table(db, from_obj);
    Value result_list = val_list();

    SObjectQueryPlan* plan = sobject_query_plan_build(table, fields, field_count, where_field, where_op);

    // Pre-size the result array: one realloc-free growth instead of log2(N).
    if (limit > 0 && limit < table->record_count) {
        ValueArray* arr = result_list.as.list_val;
        arr->items = (Value*)malloc(sizeof(Value) * (size_t)limit);
        if (!arr->items) abort();
        arr->capacity = limit;
    } else if (table->record_count > 0) {
        ValueArray* arr = result_list.as.list_val;
        arr->items = (Value*)malloc(sizeof(Value) * (size_t)table->record_count);
        if (!arr->items) abort();
        arr->capacity = table->record_count;
    }

    const char* type_name_interned = sobject_intern_field(from_obj);
    int out_count = field_count + 1; // + implicit Id

    int start_i = 0;
    int end_i = table->record_count;
    if (where_field && nadr_str_eq(where_field, "Id") && plan->where_kind == QOP_EQ && where_val.type == VAL_STRING) {
        int direct_match = table_find_by_id(table, where_val.as.string_val);
        if (direct_match < 0) {
            sobject_query_plan_free(plan);
            return result_list;
        }
        start_i = direct_match;
        end_i = direct_match + 1;
    }

    for (int i = start_i; i < end_i; i++) {
        SObject* src = table->records[i];
        if (src->is_deleted) continue;
        if (!query_row_matches(src, plan, where_val)) continue;

        // Id is always projected.
        int id_at = sobject_field_slot(src, "Id");
        if (id_at < 0) continue;

        SObject* out = sobject_new_projected(type_name_interned, out_count);

        for (int f = 0; f < field_count; f++) {
            const char* fname = fields[f];
            if (nadr_str_eq(fname, "Id")) continue; // filled below
            int at = plan->fields[f].slot;
            // Trust the resolved slot only when the record still agrees with it
            // (the plan was resolved against a sample record of this table).
            if (at < 0 || at >= src->field_count ||
                !field_name_eq(src->fields[at].name, fname)) {
                at = sobject_locate(src, fname, plan->fields[f].hash, strlen(fname));
            }
            sobject_put(out, fname, at >= 0 ? src->fields[at].value : val_null());
        }

        sobject_put(out, (char*) "Id", src->fields[id_at].value);

        val_list_add(&result_list, val_sobject(out));

        if (limit > 0 && val_list_size(&result_list) >= limit) break;
    }

    sobject_query_plan_free(plan);
    return result_list;
}

// savepoint snapshot
int mock_db_set_savepoint(void) {
    MockDB* db = mock_db_get_instance();
    if (db->savepoint_count + 1 > db->savepoint_capacity) {
        db->savepoint_capacity = db->savepoint_capacity < 4 ? 4 : db->savepoint_capacity * 2;
        db->savepoints = (DBSnapshot*)realloc(db->savepoints, sizeof(DBSnapshot) * (size_t)db->savepoint_capacity);
        if (!db->savepoints) abort();
    }
    int sp_id = db->next_savepoint_id++;
    DBSnapshot* snap = &db->savepoints[db->savepoint_count++];
    snap->savepoint_id = sp_id;
    snap->saved_table_count = db->table_count;
    snap->saved_tables = (DBTable*)malloc(sizeof(DBTable) * (size_t)(db->table_count > 0 ? db->table_count : 1));

    for (int t = 0; t < db->table_count; t++) {
        DBTable* src_t = &db->tables[t];
        DBTable* dst_t = &snap->saved_tables[t];
        dst_t->object_name = nadr_strdup(src_t->object_name);
        dst_t->record_count = src_t->record_count;
        dst_t->record_capacity = src_t->record_count;
        dst_t->auto_id_seq = src_t->auto_id_seq;
        dst_t->records = (SObject**)malloc(sizeof(SObject*) * (size_t)(src_t->record_count > 0 ? src_t->record_count : 1));
        for (int r = 0; r < src_t->record_count; r++) {
            dst_t->records[r] = src_t->records[r];
        }
    }
    return sp_id;
}

// rollback to savepoint
void mock_db_rollback(int savepoint_id) {
    MockDB* db = mock_db_get_instance();
    int idx = -1;
    for (int i = db->savepoint_count - 1; i >= 0; i--) {
        if (db->savepoints[i].savepoint_id == savepoint_id) {
            idx = i;
            break;
        }
    }
    if (idx == -1) return;

    DBSnapshot* snap = &db->savepoints[idx];

    // restore table states
    for (int t = 0; t < db->table_count; t++) {
        if (db->tables[t].records) free(db->tables[t].records);
        if (db->tables[t].object_name) free(db->tables[t].object_name);
    }
    free(db->tables);

    db->table_count = snap->saved_table_count;
    db->table_capacity = snap->saved_table_count;
    db->tables = (DBTable*)malloc(sizeof(DBTable) * (size_t)(db->table_count > 0 ? db->table_count : 1));

    for (int t = 0; t < snap->saved_table_count; t++) {
        DBTable* src_t = &snap->saved_tables[t];
        DBTable* dst_t = &db->tables[t];
        dst_t->object_name = nadr_strdup(src_t->object_name);
        dst_t->record_count = src_t->record_count;
        dst_t->record_capacity = src_t->record_count;
        dst_t->auto_id_seq = src_t->auto_id_seq;
        dst_t->records = (SObject**)malloc(sizeof(SObject*) * (size_t)(src_t->record_count > 0 ? src_t->record_count : 1));
        for (int r = 0; r < src_t->record_count; r++) {
            dst_t->records[r] = src_t->records[r];
        }
    }
}
