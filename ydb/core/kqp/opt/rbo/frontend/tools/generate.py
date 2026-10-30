#!/usr/bin/env python3
"""Generate parser grammars and the fast lexer's policy from the SQL grammar.

Only flat alternations of rule/token names are expanded. A grammar change that
introduces another form fails the build instead of silently changing acceptance.
"""
import re
import sys
from pathlib import Path


def generate(path):
    grammar = Path(path).read_text()
    grammar = re.sub(r"/\*.*?\*/|//[^\n]*", "", grammar, flags=re.S)
    rules = dict(re.findall(r"^([a-z][a-z_0-9]*):\s*(.*?);", grammar, re.M | re.S))

    def tokens(name):
        if name.isupper():
            return {name}
        body = rules[name]
        if re.search(r"[^\w\s|()]", body):
            raise ValueError(f"identifier rule is no longer a flat alternation: {name}")
        result = set()
        for child in re.findall(r"\b\w+\b", body):
            result.update(tokens(child))
        return result

    categories = [tokens(rule) for rule in ("id_expr", "id_table_or_type", "id_as_compat", "id_or_type")]
    keywords = re.findall(r"^([A-Z][A-Z_0-9]*):\s*((?:[A-Z] +)*[A-Z])\s*;", grammar, re.M)
    print("// Generated from SQLv1Antlr4.g.in. Do not edit.")
    print("static constexpr TKeyword Keywords[] = {")
    for name, spelling in sorted(keywords, key=lambda row: row[1].replace(" ", "")):
        mask = sum(1 << i for i, category in enumerate(categories) if name in category)
        print(f'    {{"{spelling.replace(" ", "")}", {mask}}},')
    print("};")


def parser_grammar(path, ansi):
    grammar = Path(path).read_text()
    name = "NativeRboAnsi" if ansi else "NativeRboDefault"
    grammar = grammar.replace("grammar SQLv1Antlr4;", f"grammar {name};", 1)
    single = "~([']) | (QUOTE_SINGLE QUOTE_SINGLE)" if ansi else r"~(['\\]) | (BACKSLASH .)"
    double = '~([\"]) | (QUOTE_DOUBLE QUOTE_DOUBLE)' if ansi else r'~(["\\]) | (BACKSLASH .)'
    for key, value in {
        "GRAMMAR_STRING_CORE_SINGLE": single,
        "GRAMMAR_STRING_CORE_DOUBLE": double,
        "GRAMMAR_MULTILINE_COMMENT_CORE": "MULTILINE_COMMENT | ." if ansi else ".",
    }.items():
        grammar = grammar.replace(f"@{key}@", value)
    print(grammar, end="")


if __name__ == "__main__":
    if sys.argv[2] == "keywords":
        generate(sys.argv[1])
    elif sys.argv[2] in ("default", "ansi"):
        parser_grammar(sys.argv[1], sys.argv[2] == "ansi")
    else:
        raise ValueError("expected keywords, default, or ansi")
