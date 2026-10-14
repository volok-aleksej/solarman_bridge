#pragma once

#include "deye_registers.hpp"

#include <cstdint>

// Every stored sample field, defined once. The SQLite schema, INSERT and
// SELECT, and the JSON field names of the HTTP API are all generated from
// this table so they cannot drift apart.
struct SampleColumn {
    const char* name;
    // Meaning, unit and source register. Also emitted as a comment into the
    // CREATE TABLE text, so `.schema samples` in sqlite3 shows it.
    const char* description;
    int64_t (*get)(const InverterSample&);
    void    (*set)(InverterSample&, int64_t);
};

extern const SampleColumn kSampleColumns[47];
