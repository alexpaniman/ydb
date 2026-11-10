#include "frontend.h"

#include <algorithm>
#include <charconv>

namespace NKikimr::NKqp::NFrontend {
namespace {
void AppendInteger(std::string& out, std::uint64_t value) {
    char digits[20];
    const auto result = std::to_chars(std::begin(digits), std::end(digits), value);
    out.append(digits, result.ptr);
}

void AppendIds(std::string& out, std::span<const std::uint32_t> ids) {
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i) out += ',';
        AppendInteger(out, ids[i]);
    }
}
}

std::string Explain(const TBoundProgram& program) {
    constexpr const char* names[]{"Unit", "Read", "Map", "Filter", "Join", "Aggregate", "Sort", "Limit", "UnionAll", "Capture", "Window", "Parameter", "Reuse", "Effect"};
    std::string out;
    out.reserve(program.Relations.size() * 64 + program.Columns.size() * 16);
    for (std::size_t i = 0; i < program.Relations.size(); ++i) {
        const auto& relation = program.Relations[i];
        AppendInteger(out, i);
        out += ": "; out += names[static_cast<unsigned>(relation.Kind)];
        if (!relation.Table.empty()) { out += ' '; out += relation.Table; }
        if (relation.Index) { out += " view="; out += *relation.Index; }
        if (!relation.Alias.empty()) { out += " alias="; out += relation.Alias; }
        out += " inputs=["; AppendIds(out, relation.Inputs);
        out += "] columns=["; AppendIds(out, relation.Columns); out += ']';
        for (const auto& capture : relation.Captures) {
            out += " capture="; AppendInteger(out, capture.Local);
            out += "<-"; AppendInteger(out, capture.Outer);
        }
        for (const auto& function : relation.WindowFunctions) {
            out += " window="; AppendInteger(out, function.Output);
            out += ':'; out += function.Function;
        }
        for (const auto& set : relation.GroupingSets) {
            out += " grouping-set=["; AppendIds(out, set); out += ']';
        }
        for (const auto& [output, input] : relation.GroupingColumns) {
            out += " grouping-output="; AppendInteger(out, output);
            out += "<-"; AppendInteger(out, input);
        }
        for (const auto& [output, input] : relation.GroupingIndicators) {
            out += " grouping-indicator="; AppendInteger(out, output);
            out += "<-"; AppendInteger(out, input);
        }
        out += '\n';
    }
    std::vector<TColumnId> bindings;
    for (const auto& [binding, subplan] : program.Subplans) bindings.push_back(binding);
    std::sort(bindings.begin(), bindings.end());
    for (const auto binding : bindings) {
        const auto& subplan = program.Subplans.at(binding);
        out += subplan.Kind == ESubquery::Exists ? "exists " : subplan.Kind == ESubquery::In ? "in " : "scalar ";
        AppendInteger(out, binding);
        out += " root="; AppendInteger(out, subplan.Root);
        out += " result="; AppendInteger(out, subplan.Result);
        if (subplan.Lookup) { out += " lookup="; AppendInteger(out, *subplan.Lookup); }
        out += '\n';
    }
    for (std::size_t i = 0; i < program.Results.size(); ++i) {
        out += "result "; AppendInteger(out, i);
        out += " root="; AppendInteger(out, program.Results[i].Root); out += ':';
        for (const auto& column : program.Results[i].Columns) {
            out += ' '; out += column.Name; out += '='; AppendInteger(out, column.Column);
            out += ':'; out += column.Type.ToString();
        }
        out += '\n';
    }
    return out;
}

} // namespace NKikimr::NKqp::NFrontend
