#pragma once
// Structured-output detection over plain text lines (no terminal dependency).
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <optional>

namespace canopy {

enum class SpanKind : uint8_t { Path, Url, Error, Warning, GitHash, Number, Email, IpAddr };

struct Span {
    SpanKind kind;
    int col_begin = 0, col_end = 0;   // [begin,end) in code points (cells), not bytes
    std::string value;                // the matched text (for Path: as written; resolve with cwd)
    int line_no = 0;                  // optional: file:LINE suffix for paths (0 if none)
};

// Scan a single line. `cwd` is used only for Path existence checks when check_fs is true.
std::vector<Span> scan_line(std::string_view line, const std::string& cwd, bool check_fs = true);

// Column-aligned table detection over a block's output lines.
struct TableInfo {
    bool is_table = false;
    int header_row = -1;              // index into lines of the header (or -1)
    int first_data_row = 0;
    std::vector<int> col_begins;      // column start offsets (code points)
    std::vector<bool> numeric;        // per column: values parse as numbers
};
TableInfo detect_table(const std::vector<std::string>& lines);
// Split one line according to table columns.
std::vector<std::string> split_columns(std::string_view line, const TableInfo& t);
// Returns a permutation of data row indices sorted by column `col` (numeric-aware).
std::vector<int> sort_rows(const std::vector<std::string>& lines, const TableInfo& t, int col, bool descending);

// JSON detection/formatting on a whole output text.
bool looks_like_json(std::string_view text);
std::optional<std::string> pretty_json(std::string_view text, int indent = 2);
std::optional<std::string> compact_json(std::string_view text);

// Numeric series from a column (for sparklines); empty if not numeric.
std::vector<double> numeric_column(const std::vector<std::string>& lines, const TableInfo& t, int col);

// Error/warning classification for gutter markers.
bool is_error_line(std::string_view line);
bool is_warning_line(std::string_view line);

} // namespace canopy
