#!/usr/bin/env python3
"""Generate the handwritten lexer's identifier policy from the SQL grammar.

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


if __name__ == "__main__":
    generate(sys.argv[1])
