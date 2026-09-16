// James Ezra Seitenschlag
// 11.09.2026
//
// nadir runtime thingy

#ifndef NADIR_SOBJECT_H
#define NADIR_SOBJECT_H

#include "common.h"
#include "value.h"

struct Value;

// field on an sobject
//
// `hash` caches the case-folded name hash and `slot` is this field's forward
// index into SObject.field_index. Together they let sobject_get resolve a
// field without strcasecmp-ing every name on the record, which used to make
// every bulk query projection O(fields^2) per row.
typedef struct SObjectField {
    char* name;
    uint32_t hash;
    int32_t slot;
    struct Value value;
} SObjectField;

// sobject record
typedef struct SObject {
    char* type_name;
    SObjectField* fields;
    int field_count;
    int field_capacity;
    bool is_deleted;
    // open-addressed field name index: holds slot+1, 0 means empty.
    // Only built lazily once a record carries enough fields to pay for it.
    int32_t* field_index;
    int field_index_capacity;
    // true when `fields[].name` is shared with the global intern table and
    // therefore must not be freed with the record.
    bool names_interned;
} SObject;

SObject* sobject_new(const char* type_name);
void sobject_put(SObject* obj, const char* field_name, struct Value val);
struct Value sobject_get(SObject* obj, const char* field_name);
SObject* sobject_clone(SObject* src);
void sobject_free(SObject* obj);
const char* get_sfdc_prefix(const char* object_name);

void format_timestamp(time_t now, char* out, size_t out_sz);

// Bulk helpers. All of these exist purely to keep per-row work out of the
// row loop; they are semantically identical to the scalar versions.
void sobject_reserve_fields(SObject* obj, int capacity);
void sobject_put_prehashed(SObject* obj, const char* n, uint32_t hash, size_t len, struct Value val);
uint32_t sobject_field_hash(const char* name);
// build a record with exactly `count` resolved field slots in one allocation
SObject* sobject_new_projected(const char* type_name, int count);
void sobject_set_field_at(SObject* obj, int at, char* name, uint32_t hash, struct Value val);

// Global field-name intern table. Apex metadata field names repeat endlessly
// across records, so identical names share one allocation instead of being
// strdup-ed once per projected row.
const char* sobject_intern_field(const char* name);
const char* sobject_intern_field_slice(const char* name, size_t len);

// Resolve a field name to a positional slot, or -1 when absent. O(1) on
// records that carry a field index, otherwise a short linear probe.
int sobject_field_slot(SObject* obj, const char* name);
// Bounds-checked positional read; returns null for missing/out-of-range slots.
struct Value sobject_field_at(SObject* obj, int slot);

// in-memory table
typedef struct DBTable {
    char* object_name;
    SObject** records;
    int record_count;
    int record_capacity;
    int auto_id_seq;
} DBTable;

// savepoint snapshot
typedef struct DBSnapshot {
    int savepoint_id;
    DBTable* saved_tables;
    int saved_table_count;
} DBSnapshot;

// simple mock database
typedef struct MockDB {
    DBTable* tables;
    int table_count;
    int table_capacity;
    DBSnapshot* savepoints;
    int savepoint_count;
    int savepoint_capacity;
    int next_savepoint_id;
} MockDB;

MockDB* mock_db_get_instance(void);
struct Value mock_db_insert(SObject* obj);
struct Value mock_db_update(SObject* obj);
struct Value mock_db_delete(SObject* obj);
struct Value mock_db_query(const char* from_obj, const char** fields, int field_count,
                           const char* where_field, const char* where_op, struct Value where_val, int limit);

// Bulk entry points. A DML statement over a list is one transaction in Apex:
// every row shares one CreatedDate/LastModifiedDate, the id sequence advances
// in one step, and validation walks the field table once per row instead of
// once per field. These are the routines the interpreter calls for list DML.
void mock_db_insert_bulk(struct SObject** records, int count);
void mock_db_update_bulk(struct SObject** records, int count);
void mock_db_delete_bulk(struct SObject** records, int count);
// Applies a whole DML statement, dispatching on the pre-resolved operation kind.
void mock_db_dml_bulk(const char* operation, struct SObject** records, int count);

// Compiled form of a SOQL query. Field and WHERE clauses are resolved to
// concrete field slots once, before the row loop, so the hot loop performs
// array indexing instead of case-insensitive name scans per row.
typedef struct SObjectFieldSlot {
    char* name;
    uint32_t hash;
    int32_t slot;
} SObjectFieldSlot;

typedef struct SObjectQueryPlan {
    int32_t where_slot;
    int where_kind;
    int field_count;
    SObjectFieldSlot fields[1];
} SObjectQueryPlan;

SObjectQueryPlan* sobject_query_plan_build(DBTable* table, const char** fields, int field_count,
                                           const char* where_field, const char* where_op);
void sobject_query_plan_free(SObjectQueryPlan* plan);

int mock_db_set_savepoint(void);
void mock_db_rollback(int savepoint_id);

#endif

