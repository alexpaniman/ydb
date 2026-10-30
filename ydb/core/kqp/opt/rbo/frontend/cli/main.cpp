#include <ydb/core/kqp/opt/rbo/frontend/frontend.h>

#include <charconv>
#include <chrono>
#include <iostream>
#include <iterator>
#include <unordered_map>

using namespace NKikimr::NKqp::NFrontend;

int main(int argc, char** argv) {
    try {
        std::unordered_map<std::string, TCatalogTable> tables;
        unsigned iterations = 1;
        bool benchmark = false;
        TCompilerSettings settings;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "--help") {
                std::cout << "Read SQL from stdin. Options:\n"
                             "  --table NAME COLUMNS   comma-separated name:type schema (Decimal commas allowed)\n"
                             "  --bench N              parse+bind N times and report average time\n"
                             "  --ansi                 ANSI string escaping and nested comments\n"
                             "  --parser NAME          fast (default), grammar, handwritten, verify\n"
                             "  --mode NAME            required (default), auto, legacy\n"
                             "  --support              print the execution profile\n";
                return 0;
            }
            if (arg == "--ansi") { settings.Parsing.AnsiQuotes = true; continue; }
            if (arg == "--support") { std::cout << SupportManifest() << '\n'; return 0; }
            if (arg == "--parser" && i + 1 < argc) {
                const std::string_view value = argv[++i];
                if (value == "fast") settings.Parser = EParser::Fast;
                else if (value == "grammar") settings.Parser = EParser::Grammar;
                else if (value == "handwritten") settings.Parser = EParser::Handwritten;
                else if (value == "verify") settings.Parser = EParser::Verify;
                else throw std::runtime_error("unknown parser");
                continue;
            }
            if (arg == "--mode" && i + 1 < argc) {
                const std::string_view value = argv[++i];
                if (value == "required") settings.Mode = ECompilerMode::NativeRequired;
                else if (value == "auto") settings.Mode = ECompilerMode::Auto;
                else if (value == "legacy") settings.Mode = ECompilerMode::Legacy;
                else throw std::runtime_error("unknown compiler mode");
                continue;
            }
            if (arg == "--bench" && i + 1 < argc) {
                const std::string_view text = argv[++i];
                auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), iterations);
                if (error != std::errc() || end != text.data() + text.size() || !iterations) throw std::runtime_error("invalid iteration count");
                benchmark = true;
                continue;
            }
            if (arg == "--table" && i + 2 < argc) {
                TCatalogTable table{argv[++i], {}};
                std::string_view columns = argv[++i];
                while (!columns.empty()) {
                    std::size_t end = 0;
                    unsigned depth = 0;
                    for (; end < columns.size(); ++end) {
                        if (columns[end] == '(') ++depth;
                        if (columns[end] == ')' && depth) --depth;
                        if (columns[end] == ',' && !depth) break;
                    }
                    const auto column = columns.substr(0, end);
                    const auto colon = column.find(':');
                    if (colon == std::string_view::npos || colon == 0) throw std::runtime_error("expected column:type");
                    table.Columns.push_back({std::string(column.substr(0, colon)), ParseType(column.substr(colon + 1))});
                    columns.remove_prefix(std::min(columns.size(), end + 1));
                }
                if (!tables.emplace(table.Name, table).second) throw std::runtime_error("duplicate table schema");
                continue;
            }
            throw std::runtime_error("unknown or incomplete option: " + std::string(arg));
        }
        const std::string sql{std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>()};
        const TTableResolver resolve = [&](std::string_view name, TSourcePosition pos) {
            const auto it = tables.find(std::string(name));
            if (it == tables.end()) throw TFrontendError(pos, "unknown table: " + std::string(name));
            return it->second;
        };
        auto result = CompileNative(sql, resolve, settings); // warmup and diagnostics
        if (result.UsesLegacy()) {
            std::cout << "compiler=legacy-yql";
            if (result.Unsupported) std::cout << " feature=" << FeatureName(result.Unsupported->Feature)
                << " position=" << result.Unsupported->Position.Line << ':' << result.Unsupported->Position.Column
                << " reason=" << result.Unsupported->Message;
            std::cout << "\nCLI reports the routing decision; it does not execute the legacy compiler.\n";
            return 0;
        }
        const auto& program = result.Program->Get();
        if (!benchmark) std::cout << Explain(program);
        else {
            std::size_t expressions = 0, relations = 0;
            const auto start = std::chrono::steady_clock::now();
            for (unsigned i = 0; i < iterations; ++i) {
                auto result = CompileNative(sql, resolve, settings);
                const auto& compiled = result.Program->Get();
                expressions += compiled.Expressions.Size();
                relations += compiled.Relations.size();
            }
            const auto elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
            std::cout << "parse_bind_us=" << elapsed / iterations << " iterations=" << iterations
                      << " bytes=" << sql.size() << " expressions=" << expressions / iterations
                      << " relations=" << relations / iterations << '\n';
        }
        return 0;
    } catch (const TFrontendError& error) {
        std::cerr << error.Position.Line << ':' << error.Position.Column << ": " << error.what() << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    return 1;
}
