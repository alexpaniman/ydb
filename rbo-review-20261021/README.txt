Start with bugs.txt: the human-readable correctness fix list.
Code-author and introducing-change attribution for C01-C50 is in code-attribution.txt, with machine-readable JSON and CSV alongside it. It distinguishes source introductions, interactions, exposure changes, and uncertain lineage. Historical revisions were not runtime-bisected.
Optimization quality is separate in optimization-quality.txt.
Read scope-and-verification.txt for coverage, verification strength and limits.
Additional-candidates.txt contains issues excluded from the confirmed list.

bugs.json is the machine-readable fix list. coverage.json and rule-and-stage-coverage.csv map review work. Sanitized outcomes and compositional generators support follow-up fixes without storing exact reproduction inputs. Reviewer notes contain source reasoning, not generated cases.

The generators expect the current-checkout kqprun binary and Math UDF. Build with the project-standard relwithdebinfo commands if necessary. Run their --help entry points for options; generated inputs use anonymous memory. Local test-server execution requires permission to bind loopback ports in restricted environments.

No project sources were changed; fixes remain to be implemented.
