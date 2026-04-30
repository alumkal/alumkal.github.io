#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct TableEntry {
    int group_index = 0;
    std::string table;
};

struct SearchResult {
    bool found = false;
    int table_index = -1;
    std::vector<int> positions;
    std::vector<char> values;
    std::uint64_t representative_sets_checked = 0;
    std::uint64_t candidates_seen = 0;
};

struct SearchOptions {
    bool random = false;
    std::uint64_t seed = 0;
};

struct EvalScratch {
    std::vector<std::uint32_t> counts;
    std::vector<int> first_table;
    std::vector<int> touched;

    EvalScratch(int order, int max_clues) {
        std::size_t key_space = 1;
        for (int i = 0; i < max_clues; ++i) {
            key_space *= static_cast<std::size_t>(order);
        }
        counts.assign(key_space, 0);
        first_table.assign(key_space, -1);
    }
};

int symbol_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 36;
    }
    return -1;
}

std::vector<TableEntry> load_indexed_tables(const std::string& path, int& group_count, int& order) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("could not open " + path);
    }

    std::vector<TableEntry> entries;
    int group_index = 0;
    std::string table;
    while (in >> group_index >> table) {
        if (group_index <= 0) {
            throw std::runtime_error("group index must be positive");
        }
        if (order == 0) {
            for (int n = 1; n <= 62; ++n) {
                if (static_cast<int>(table.size()) == n * n) {
                    order = n;
                    break;
                }
            }
            if (order == 0) {
                throw std::runtime_error("first table length is not a square <= 62");
            }
        }
        if (static_cast<int>(table.size()) != order * order) {
            throw std::runtime_error("bad table line length: " + std::to_string(table.size()));
        }
        for (char c : table) {
            int value = symbol_value(c);
            if (value < 0 || value >= order) {
                throw std::runtime_error("bad table symbol");
            }
        }

        group_count = std::max(group_count, group_index);
        entries.push_back({group_index - 1, table});
    }

    if (entries.empty()) {
        throw std::runtime_error("no indexed tables loaded");
    }
    return entries;
}

std::vector<int> candidate_positions(int order) {
    std::vector<int> positions;
    for (int i = 1; i < order; ++i) {
        for (int j = 1; j < order; ++j) {
            positions.push_back(i * order + j);
        }
    }
    return positions;
}

std::vector<int> normalized_positions(const std::vector<int>& positions, int order) {
    std::vector<int> relabel(order, 0);
    int next_label = 1;
    std::vector<int> normalized;
    normalized.reserve(positions.size());

    for (int pos : positions) {
        int row = pos / order;
        int col = pos % order;

        if (relabel[row] == 0) {
            relabel[row] = next_label++;
        }
        if (relabel[col] == 0) {
            relabel[col] = next_label++;
        }

        normalized.push_back(relabel[row] * order + relabel[col]);
    }

    std::sort(normalized.begin(), normalized.end());
    return normalized;
}

bool is_symmetry_representative(const std::vector<int>& positions, int order) {
    return normalized_positions(positions, order) == positions;
}

bool should_replace_result(
    SearchResult& current,
    const SearchOptions& options,
    std::mt19937_64& rng
) {
    if (!current.found) {
        return true;
    }
    if (!options.random) {
        return false;
    }

    std::uniform_int_distribution<std::uint64_t> distribution(0, current.candidates_seen - 1);
    return distribution(rng) == 0;
}

void evaluate_positions(
    const std::vector<TableEntry>& entries,
    int order,
    const std::vector<int>& positions,
    std::vector<SearchResult>& results,
    EvalScratch& scratch,
    const SearchOptions& options,
    std::mt19937_64& rng
) {
    scratch.touched.clear();

    for (int table_idx = 0; table_idx < static_cast<int>(entries.size()); ++table_idx) {
        int key = 0;
        const std::string& table = entries[table_idx].table;
        for (int pos : positions) {
            key = key * order + symbol_value(table[pos]);
        }

        if (scratch.counts[key] == 0) {
            scratch.first_table[key] = table_idx;
            scratch.touched.push_back(key);
        }
        ++scratch.counts[key];
    }

    for (int key : scratch.touched) {
        if (scratch.counts[key] == 1) {
            int table_idx = scratch.first_table[key];
            int group_index = entries[table_idx].group_index;
            SearchResult& result = results[group_index];
            ++result.candidates_seen;

            SearchResult candidate;
            candidate.found = true;
            candidate.table_index = table_idx;
            candidate.positions = positions;
            candidate.candidates_seen = result.candidates_seen;
            for (int pos : positions) {
                candidate.values.push_back(entries[table_idx].table[pos]);
            }

            if (should_replace_result(result, options, rng)) {
                result = candidate;
            }
        }

        scratch.counts[key] = 0;
    }
}

void search_combinations(
    const std::vector<TableEntry>& entries,
    int order,
    const std::vector<int>& candidates,
    int target_size,
    int start,
    std::vector<int>& current,
    std::vector<SearchResult>& current_results,
    EvalScratch& scratch,
    const SearchOptions& options,
    std::mt19937_64& rng,
    std::uint64_t& raw_sets_seen,
    std::uint64_t& representative_sets_checked
) {
    if (static_cast<int>(current.size()) == target_size) {
        ++raw_sets_seen;
        if (!is_symmetry_representative(current, order)) {
            return;
        }
        ++representative_sets_checked;
        evaluate_positions(entries, order, current, current_results, scratch, options, rng);
        return;
    }

    int remaining = target_size - static_cast<int>(current.size());
    for (int idx = start; idx <= static_cast<int>(candidates.size()) - remaining; ++idx) {
        current.push_back(candidates[idx]);
        search_combinations(
            entries,
            order,
            candidates,
            target_size,
            idx + 1,
            current,
            current_results,
            scratch,
            options,
            rng,
            raw_sets_seen,
            representative_sets_checked
        );
        current.pop_back();
    }
}

std::vector<SearchResult> find_minimal_by_group(
    const std::vector<TableEntry>& entries,
    int order,
    int group_count,
    int max_clues,
    const SearchOptions& options,
    std::mt19937_64& rng
) {
    std::vector<int> candidates = candidate_positions(order);
    std::vector<SearchResult> final_results(group_count);
    EvalScratch scratch(order, max_clues);

    for (int clue_count = 1; clue_count <= max_clues; ++clue_count) {
        std::vector<SearchResult> current_results(group_count);
        std::vector<int> current;
        std::uint64_t raw_sets_seen = 0;
        std::uint64_t representative_sets_checked = 0;

        search_combinations(
            entries,
            order,
            candidates,
            clue_count,
            0,
            current,
            current_results,
            scratch,
            options,
            rng,
            raw_sets_seen,
            representative_sets_checked
        );

        std::cout << "clues=" << clue_count
                  << " raw_position_sets=" << raw_sets_seen
                  << " symmetry_representatives=" << representative_sets_checked
                  << '\n';

        for (int group = 0; group < group_count; ++group) {
            if (!final_results[group].found && current_results[group].found) {
                current_results[group].representative_sets_checked = representative_sets_checked;
                final_results[group] = current_results[group];
                std::cout << "  found minimal puzzle for group_index="
                          << (group + 1) << " with " << clue_count << " clues\n";
            }
        }

        bool all_found = true;
        for (const SearchResult& result : final_results) {
            all_found = all_found && result.found;
        }
        if (all_found) {
            break;
        }
    }

    return final_results;
}

void print_grid(const std::string& table, int order, const std::vector<int>& clue_positions) {
    std::vector<bool> shown(order * order, false);
    for (int i = 0; i < order; ++i) {
        shown[i] = true;
        shown[i * order] = true;
    }
    for (int pos : clue_positions) {
        shown[pos] = true;
    }

    for (int i = 0; i < order; ++i) {
        for (int j = 0; j < order; ++j) {
            int pos = i * order + j;
            std::cout << (shown[pos] ? table[pos] : '.');
            if (j + 1 != order) {
                std::cout << ' ';
            }
        }
        std::cout << '\n';
    }
}

void print_solution(const std::string& table, int order) {
    for (int i = 0; i < order; ++i) {
        for (int j = 0; j < order; ++j) {
            std::cout << table[i * order + j];
            if (j + 1 != order) {
                std::cout << ' ';
            }
        }
        std::cout << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::string path = "cayley_tables_order8_indexed.txt";
        int max_clues = 6;
        SearchOptions options;

        for (int arg = 1; arg < argc; ++arg) {
            std::string value = argv[arg];
            if (value == "--random") {
                options.random = true;
            } else if (value == "--seed") {
                if (arg + 1 >= argc) {
                    throw std::runtime_error("--seed requires a value");
                }
                options.seed = std::stoull(argv[++arg]);
            } else if (value.rfind("--seed=", 0) == 0) {
                options.seed = std::stoull(value.substr(std::string("--seed=").size()));
            } else if (value == "--max-clues") {
                if (arg + 1 >= argc) {
                    throw std::runtime_error("--max-clues requires a value");
                }
                max_clues = std::stoi(argv[++arg]);
            } else if (value.rfind("--max-clues=", 0) == 0) {
                max_clues = std::stoi(value.substr(std::string("--max-clues=").size()));
            } else if (!value.empty() && value[0] == '-') {
                throw std::runtime_error("unknown flag: " + value);
            } else {
                path = value;
            }
        }
        if (max_clues <= 0 || max_clues > 8) {
            throw std::runtime_error("max_clues must be in 1..8");
        }

        int group_count = 0;
        int order = 0;
        const std::vector<TableEntry> entries = load_indexed_tables(path, group_count, order);
        std::vector<int> group_sizes(group_count, 0);
        for (const TableEntry& entry : entries) {
            ++group_sizes[entry.group_index];
        }

        std::cout << "Loaded " << entries.size() << " indexed order-" << order << " tables from "
                  << path << ".\n";
        std::cout << "Group counts:\n";
        for (int group = 0; group < group_count; ++group) {
            std::cout << "  " << (group + 1) << ": " << group_sizes[group] << '\n';
        }
        if (options.random) {
            if (options.seed == 0) {
                std::random_device rd;
                options.seed = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
            }
            std::cout << "Selection: random minimal puzzle (seed=" << options.seed << ").\n";
        } else {
            std::cout << "Selection: first minimal puzzle found.\n";
        }
        std::mt19937_64 rng(options.seed);

        std::vector<SearchResult> results = find_minimal_by_group(
            entries,
            order,
            group_count,
            max_clues,
            options,
            rng
        );

        bool all_found = true;
        for (int group = 0; group < group_count; ++group) {
            all_found = all_found && results[group].found;
        }
        if (!all_found) {
            std::cout << "Not every group index has a unique puzzle with at most "
                      << max_clues << " clues.\n";
        }

        for (int group = 0; group < group_count; ++group) {
            const SearchResult& result = results[group];
            std::cout << "\n=== group_index=" << (group + 1) << " ===\n";
            if (!result.found) {
                std::cout << "No puzzle found within the clue limit.\n";
                continue;
            }

            const TableEntry& entry = entries[result.table_index];
            std::cout << "Minimal clues: " << result.positions.size() << '\n';
            std::cout << "Solution table row: " << result.table_index << '\n';
            std::cout << "Symmetry representatives checked at this clue count: "
                      << result.representative_sets_checked << '\n';
            if (options.random) {
                std::cout << "Minimal singleton candidates seen: "
                          << result.candidates_seen << '\n';
            }
            std::cout << "Clues, zero-based row/column/value:\n";
            for (std::size_t idx = 0; idx < result.positions.size(); ++idx) {
                int pos = result.positions[idx];
                std::cout << "  (" << (pos / order) << ", " << (pos % order)
                          << ") = " << result.values[idx] << '\n';
            }

            std::cout << "\nPuzzle (. = blank; row 0 and column 0 are given):\n";
            print_grid(entry.table, order, result.positions);

            std::cout << "\nSolution:\n";
            print_solution(entry.table, order);
        }

        return all_found ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
