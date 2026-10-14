#pragma once

#include "deye_registers.hpp"

#include <cstdint>

// Every sample field, defined once. The SQLite schema, INSERT and SELECT,
// and the JSON field names of the HTTP API are all generated from these
// tables so they cannot drift apart.
struct SampleColumn {
    const char* name;
    // Meaning, unit and source register. Also emitted as a comment into the
    // CREATE TABLE text, so `.schema samples` in sqlite3 shows it.
    const char* description;
    int64_t (*get)(const InverterSample&);
    void    (*set)(InverterSample&, int64_t);
};

// Stored in the samples table and returned by api/history.
extern const SampleColumn kSampleColumns[35];
// Live only: returned by api/live, never stored per sample.
extern const SampleColumn kCounterColumns[12];
