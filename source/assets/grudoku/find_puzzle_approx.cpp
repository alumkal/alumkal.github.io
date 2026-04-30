#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace {

struct SearchOptions {
    std::string table_path;
    int max_attempts = 100;
    std::string output_prefix;
};

struct LoadedCorpus {
    int order = 0;
    int table_size = 0;
    int group_count = 0;
    std::vector<int> group_sizes;
    std::vector<int> group_ids;
    std::vector<std::uint8_t> col_major_tables;
    std::vector<int> sampled_table_index_by_group;
};

struct Attempt {
    std::vector<int> offsets;
};

struct PuzzleResult {
    bool found = false;
    int table_index = -1;
    int clue_count = 0;
    std::vector<int> offsets;
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

char symbol_char(int value) {
    if (value >= 0 && value <= 9) {
        return static_cast<char>('0' + value);
    }
    if (value >= 10 && value < 36) {
        return static_cast<char>('A' + (value - 10));
    }
    if (value >= 36 && value < 62) {
        return static_cast<char>('a' + (value - 36));
    }
    throw std::runtime_error("table value outside supported alphabet");
}

SearchOptions parse_args(int argc, char** argv) {
    SearchOptions options;
    for (int index = 1; index < argc; ++index) {
        std::string value = argv[index];
        if (value == "--table") {
            if (index + 1 >= argc) {
                throw std::runtime_error("--table requires a value");
            }
            options.table_path = argv[++index];
        } else if (value.rfind("--table=", 0) == 0) {
            options.table_path = value.substr(std::string("--table=").size());
        } else if (value == "--max-attempts") {
            if (index + 1 >= argc) {
                throw std::runtime_error("--max-attempts requires a value");
            }
            options.max_attempts = std::stoi(argv[++index]);
        } else if (value.rfind("--max-attempts=", 0) == 0) {
            options.max_attempts = std::stoi(value.substr(std::string("--max-attempts=").size()));
        } else if (value == "--output-prefix") {
            if (index + 1 >= argc) {
                throw std::runtime_error("--output-prefix requires a value");
            }
            options.output_prefix = argv[++index];
        } else if (value.rfind("--output-prefix=", 0) == 0) {
            options.output_prefix = value.substr(std::string("--output-prefix=").size());
        } else {
            throw std::runtime_error("unknown flag: " + value);
        }
    }

    if (options.table_path.empty()) {
        throw std::runtime_error("--table is required");
    }
    if (options.max_attempts <= 0) {
        throw std::runtime_error("max_attempts must be positive");
    }
    return options;
}

void validate_table_shape(const std::string& table, int& order) {
    if (order == 0) {
        for (int candidate = 1; candidate <= 62; ++candidate) {
            if (static_cast<int>(table.size()) == candidate * candidate) {
                order = candidate;
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
}

LoadedCorpus load_corpus(const SearchOptions& options, std::uint64_t seed) {
    std::ifstream in(options.table_path);
    if (!in) {
        throw std::runtime_error("could not open " + options.table_path);
    }

    LoadedCorpus corpus;
    std::mt19937_64 rng(seed);
    std::vector<int> seen_by_group;

    int raw_group = 0;
    std::string table;
    while (in >> raw_group >> table) {
        if (raw_group <= 0) {
            throw std::runtime_error("group index must be positive");
        }
        validate_table_shape(table, corpus.order);
        if (corpus.table_size == 0) {
            corpus.table_size = corpus.order * corpus.order;
        }

        int group = raw_group - 1;
        if (raw_group > corpus.group_count) {
            corpus.group_count = raw_group;
            corpus.group_sizes.resize(corpus.group_count, 0);
            corpus.sampled_table_index_by_group.resize(corpus.group_count, -1);
            seen_by_group.resize(corpus.group_count, 0);
        }

        const int table_index = static_cast<int>(corpus.group_ids.size());
        corpus.group_ids.push_back(group);
        ++corpus.group_sizes[group];

        const std::size_t base = corpus.col_major_tables.size();
        corpus.col_major_tables.resize(base + static_cast<std::size_t>(corpus.table_size));
        for (int row = 0; row < corpus.order; ++row) {
            for (int col = 0; col < corpus.order; ++col) {
                int value = symbol_value(table[row * corpus.order + col]);
                corpus.col_major_tables[base + col * corpus.order + row] = static_cast<std::uint8_t>(value);
            }
        }

        int seen = seen_by_group[group]++;
        if (corpus.sampled_table_index_by_group[group] == -1) {
            corpus.sampled_table_index_by_group[group] = table_index;
        } else {
            std::uniform_int_distribution<int> distribution(0, seen);
            if (distribution(rng) == 0) {
                corpus.sampled_table_index_by_group[group] = table_index;
            }
        }
    }

    if (corpus.group_ids.empty()) {
        throw std::runtime_error("no indexed tables loaded");
    }
    return corpus;
}

std::size_t table_base(const LoadedCorpus& corpus, int table_index) {
    return static_cast<std::size_t>(table_index) * static_cast<std::size_t>(corpus.table_size);
}

std::vector<int> interior_offsets(const LoadedCorpus& corpus) {
    std::vector<int> offsets;
    offsets.reserve((corpus.order - 1) * (corpus.order - 1));
    for (int row = 1; row < corpus.order; ++row) {
        for (int col = 1; col < corpus.order; ++col) {
            offsets.push_back(col * corpus.order + row);
        }
    }
    return offsets;
}

std::vector<Attempt> make_attempts(
    const std::vector<int>& all_offsets,
    int clue_count,
    int max_attempts,
    std::mt19937_64& rng
) {
    std::vector<Attempt> attempts;
    attempts.reserve(max_attempts);
    std::vector<int> shuffled = all_offsets;
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        std::shuffle(shuffled.begin(), shuffled.end(), rng);
        std::vector<int> offsets(shuffled.begin(), shuffled.begin() + clue_count);
        std::sort(offsets.begin(), offsets.end());
        attempts.push_back({std::move(offsets)});
    }
    return attempts;
}

bool is_unique_attempt(const LoadedCorpus& corpus, int target_table_index, const Attempt& attempt) {
    const std::size_t target = table_base(corpus, target_table_index);
    int matches = 0;
#ifdef _OPENMP
#pragma omp parallel for reduction(+ : matches)
#endif
    for (int table_index = 0; table_index < static_cast<int>(corpus.group_ids.size()); ++table_index) {
        const std::size_t base = table_base(corpus, table_index);
        bool same = true;
        for (int offset : attempt.offsets) {
            if (corpus.col_major_tables[target + offset] != corpus.col_major_tables[base + offset]) {
                same = false;
                break;
            }
        }
        matches += same ? 1 : 0;
    }
    return matches == 1;
}

int find_unique_attempt(const LoadedCorpus& corpus, int target_table_index, const std::vector<Attempt>& attempts) {
    for (std::size_t index = 0; index < attempts.size(); ++index) {
        if (is_unique_attempt(corpus, target_table_index, attempts[index])) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

PuzzleResult bisect_clues(
    const LoadedCorpus& corpus,
    int target_table_index,
    const std::vector<int>& offsets,
    int max_attempts,
    std::mt19937_64& rng
) {
    PuzzleResult best;
    best.table_index = target_table_index;
    int low = 1;
    int high = static_cast<int>(offsets.size());

    while (low <= high) {
        int mid = low + (high - low) / 2;
        std::vector<Attempt> attempts = make_attempts(offsets, mid, max_attempts, rng);
        int winner = find_unique_attempt(corpus, target_table_index, attempts);
        if (winner >= 0) {
            best.found = true;
            best.clue_count = mid;
            best.offsets = attempts[winner].offsets;
            high = mid - 1;
        } else {
            low = mid + 1;
        }
    }

    return best;
}

std::string render_puzzle(const LoadedCorpus& corpus, int table_index, const std::vector<int>& clue_offsets) {
    std::vector<bool> shown(static_cast<std::size_t>(corpus.table_size), false);
    for (int value = 0; value < corpus.order; ++value) {
        shown[value] = true;
        shown[static_cast<std::size_t>(value) * static_cast<std::size_t>(corpus.order)] = true;
    }
    for (int offset : clue_offsets) {
        shown[static_cast<std::size_t>(offset)] = true;
    }

    const std::size_t base = table_base(corpus, table_index);
    std::string out;
    for (int row = 0; row < corpus.order; ++row) {
        for (int col = 0; col < corpus.order; ++col) {
            const int offset = col * corpus.order + row;
            out.push_back(shown[static_cast<std::size_t>(offset)] ? symbol_char(corpus.col_major_tables[base + offset]) : '.');
            if (col + 1 != corpus.order) {
                out.push_back(' ');
            }
        }
        out.push_back('\n');
    }
    return out;
}

void dump_puzzle(const SearchOptions& options, int type_id, const std::string& content) {
    if (options.output_prefix.empty()) {
        return;
    }
    std::filesystem::path path = options.output_prefix + "_" + std::to_string(type_id) + ".txt";
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("could not write " + path.string());
    }
    out << content;
}

} // namespace

int main(int argc, char** argv) {
    try {
        SearchOptions options = parse_args(argc, argv);
        std::random_device rd;
        const std::uint64_t seed = (static_cast<std::uint64_t>(rd()) << 32) ^ rd();
        std::mt19937_64 rng(seed);

        LoadedCorpus corpus = load_corpus(options, seed ^ 0x9E3779B97F4A7C15ULL);
        std::cout << "Loaded " << corpus.group_ids.size() << " tables from " << options.table_path
                  << " (order=" << corpus.order << ", groups=" << corpus.group_count << ")\n";
        std::cout << "Using max_attempts=" << options.max_attempts << ", seed=" << seed << "\n";

        std::vector<int> offsets = interior_offsets(corpus);
        for (int group = 0; group < corpus.group_count; ++group) {
            std::cout << "\n=== type_id=" << (group + 1) << " ===\n";
            int table_index = corpus.sampled_table_index_by_group[group];
            if (table_index < 0) {
                std::cout << "No table available for this type.\n";
                continue;
            }

            PuzzleResult result = bisect_clues(corpus, table_index, offsets, options.max_attempts, rng);
            if (!result.found) {
                std::cout << "No unique puzzle found within the random attempt budget.\n";
                continue;
            }

            std::cout << "table_index: " << table_index << "\n";
            std::cout << "clues: " << result.clue_count << "\n";
            std::cout << "clue positions (zero-based row,col):\n";
            for (int offset : result.offsets) {
                int col = offset / corpus.order;
                int row = offset % corpus.order;
                std::cout << "  (" << row << ", " << col << ")\n";
            }

            std::string puzzle = render_puzzle(corpus, table_index, result.offsets);
            std::cout << "\n" << puzzle;
            dump_puzzle(options, group + 1, puzzle);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
